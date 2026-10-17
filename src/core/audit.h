// Colaimdation - audit log.
//
// Two layers, deliberately:
//   * an in-memory ring of fully materialised records, used for queries and for
//     resolving rollback targets, and
//   * an append-only JSONL file on disk, used as the durable trail the user can
//     inspect outside the app.
//
// File I/O never runs on the UI thread: AuditLog owns a small writer thread and
// a bounded queue, so a slow disk cannot stall editing.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/document.h"
#include "core/json.h"

namespace colai {

enum class AuditStatus { Applied, Rejected, Previewed, RolledBack, Saved, Opened, Info };

std::string_view audit_status_name(AuditStatus status);

struct AuditRecord {
    int64_t id = 0;                  // monotonic, assigned by the log
    uint64_t document_sequence = 0;
    std::string document_id;
    std::string revision_before;
    std::string revision_after;
    EditSource source = EditSource::Agent;
    std::string tool;
    std::string session;
    std::string summary;
    AuditStatus status = AuditStatus::Applied;
    std::string error_code;
    std::string error_message;
    std::vector<TextChange> changes;
    std::string diff;              // unified diff, computed on demand
    int64_t timestamp_ms = 0;
    size_t bytes_before = 0;
    size_t bytes_after = 0;
    bool reversible = false;

    Json to_json(bool with_diff = false) const;
};

struct AuditQuery {
    size_t limit = 50;
    std::string document_id;
    std::string session;
    std::string tool;
    std::optional<EditSource> source;
    std::optional<AuditStatus> status;
    int64_t since_ms = 0;
    bool with_diff = false;
};

class AuditLog {
public:
    AuditLog() = default;
    ~AuditLog();

    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    // Opens (or creates) the JSONL file. Safe to call again to rotate.
    bool open_file(const std::string& path, std::string* error = nullptr);
    void close_file();
    const std::string& file_path() const { return file_path_; }
    bool file_enabled() const { return !file_path_.empty(); }

    int64_t record(AuditRecord record);
    void flush();
    void stop();

    // In-memory queries (newest first when `newest_first` is true).
    std::vector<AuditRecord> query(const AuditQuery& query, bool newest_first = true) const;
    const AuditRecord* find(int64_t id) const;
    size_t size() const { return records_.size(); }
    int64_t last_id() const { return last_id_; }

    // Retention.
    void set_capacity(size_t records, size_t max_file_bytes);
    void clear();

private:
    void writer_loop();
    void write_line(const std::string& line);
    void rotate_if_needed_locked(std::string& line_out);

    std::deque<AuditRecord> records_;
    size_t capacity_ = 2000;
    size_t max_file_bytes_ = 32ull * 1024 * 1024;
    int64_t last_id_ = 0;

    std::string file_path_;
    void* file_handle_ = nullptr; // HANDLE
    size_t file_bytes_ = 0;

    struct Queue;
    Queue* queue_ = nullptr;
};

} // namespace colai
