#include "core/audit.h"

#include "core/diff.h"
#include "core/utf8.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace colai {

std::string_view audit_status_name(AuditStatus status) {
    switch (status) {
    case AuditStatus::Applied: return "applied";
    case AuditStatus::Rejected: return "rejected";
    case AuditStatus::Previewed: return "previewed";
    case AuditStatus::RolledBack: return "rolled_back";
    case AuditStatus::Saved: return "saved";
    case AuditStatus::Opened: return "opened";
    case AuditStatus::Info: return "info";
    }
    return "info";
}

namespace {
std::optional<AuditStatus> audit_status_from_name(std::string_view name) {
    const std::string lower = utf8::to_lower(name);
    if (lower == "applied") return AuditStatus::Applied;
    if (lower == "rejected") return AuditStatus::Rejected;
    if (lower == "previewed") return AuditStatus::Previewed;
    if (lower == "rolled_back" || lower == "rolledback") return AuditStatus::RolledBack;
    if (lower == "saved") return AuditStatus::Saved;
    if (lower == "opened") return AuditStatus::Opened;
    if (lower == "info") return AuditStatus::Info;
    return std::nullopt;
}
} // namespace

struct AuditLog::Queue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> pending;
    std::atomic<bool> shutdown{false};
    std::thread worker;
};

Json AuditRecord::to_json(bool with_diff) const {
    Json out = Json::object();
    out.set("id", Json(static_cast<int64_t>(id)));
    out.set("document_sequence", Json(static_cast<int64_t>(document_sequence)));
    out.set("document_id", Json(document_id));
    out.set("revision_before", Json(revision_before));
    out.set("revision_after", Json(revision_after));
    out.set("source", Json(std::string(edit_source_name(source))));
    out.set("tool", Json(tool));
    if (!session.empty()) out.set("session", Json(session));
    out.set("summary", Json(summary));
    out.set("status", Json(std::string(audit_status_name(status))));
    if (!error_code.empty()) {
        Json error = Json::object();
        error.set("code", Json(error_code));
        error.set("message", Json(error_message));
        out.set("error", std::move(error));
    }
    out.set("timestamp_ms", Json(timestamp_ms));
    out.set("timestamp", Json(iso8601(timestamp_ms)));
    out.set("bytes_before", Json(static_cast<int64_t>(bytes_before)));
    out.set("bytes_after", Json(static_cast<int64_t>(bytes_after)));
    out.set("byte_delta", Json(static_cast<int64_t>(bytes_after) - static_cast<int64_t>(bytes_before)));
    out.set("reversible", Json(reversible));
    Json change_list = Json::array();
    for (const TextChange& change : changes) {
        Json entry = Json::object();
        entry.set("byte_start", Json(static_cast<int64_t>(change.begin)));
        entry.set("byte_end", Json(static_cast<int64_t>(change.end)));
        entry.set("inserted_bytes", Json(static_cast<int64_t>(change.inserted.size())));
        entry.set("inserted", Json(change.inserted));
        change_list.push(std::move(entry));
    }
    out.set("changes", std::move(change_list));
    if (with_diff && !diff.empty()) out.set("diff", Json(diff));
    return out;
}

AuditLog::~AuditLog() {
    stop();
    close_file();
}

bool AuditLog::open_file(const std::string& path, std::string* error) {
    close_file();
    if (path.empty()) return true;
    const std::wstring wide = utf8::to_wide(path);
    HANDLE handle = ::CreateFileW(wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "cannot open audit log '" + path + "' (error " +
                     std::to_string(::GetLastError()) + ")";
        }
        return false;
    }
    LARGE_INTEGER size{};
    ::GetFileSizeEx(handle, &size);
    file_handle_ = handle;
    file_path_ = path;
    file_bytes_ = static_cast<size_t>(size.QuadPart);
    return true;
}

void AuditLog::close_file() {
    flush();
    if (file_handle_) {
        ::CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
    file_path_.clear();
    file_bytes_ = 0;
}

void AuditLog::write_line(const std::string& line) {
    if (!file_handle_) return;
    HANDLE handle = static_cast<HANDLE>(file_handle_);
    DWORD written = 0;
    ::WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    file_bytes_ += written;
}

void AuditLog::rotate_if_needed_locked(std::string& line_out) {
    if (!file_handle_ || max_file_bytes_ == 0) return;
    if (file_bytes_ + line_out.size() <= max_file_bytes_) return;
    // Rotate: close, rename to ".1", reopen fresh.
    ::CloseHandle(static_cast<HANDLE>(file_handle_));
    file_handle_ = nullptr;
    const std::wstring wide = utf8::to_wide(file_path_);
    const std::wstring backup = wide + L".1";
    ::DeleteFileW(backup.c_str());
    ::MoveFileW(wide.c_str(), backup.c_str());
    HANDLE handle = ::CreateFileW(wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    file_handle_ = handle;
    if (handle != INVALID_HANDLE_VALUE) {
        // The fresh file starts empty, but the line that triggered the rotation
        // still has to be counted, otherwise the size estimate drifts and the
        // log can end up rotating on every single write.
        file_bytes_ = line_out.size();
    } else {
        file_bytes_ = 0;
    }
}

void AuditLog::writer_loop() {
    for (;;) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(queue_->mutex);
            queue_->cv.wait_for(lock, std::chrono::milliseconds(400), [this] {
                return queue_->shutdown.load() || !queue_->pending.empty();
            });
            batch.swap(queue_->pending);
            if (queue_->shutdown.load() && batch.empty()) return;
        }
        for (const std::string& line : batch) {
            rotate_if_needed_locked(const_cast<std::string&>(line));
            write_line(line);
        }
        if (file_handle_) ::FlushFileBuffers(static_cast<HANDLE>(file_handle_));
    }
}

int64_t AuditLog::record(AuditRecord entry) {
    entry.id = ++last_id_;
    if (entry.timestamp_ms == 0) entry.timestamp_ms = now_ms();
    if (entry.diff.empty() && !entry.changes.empty()) {
        // The diff summary is derived from the recorded changes; the caller may
        // replace it with a richer one.
    }

    records_.push_back(entry);
    while (records_.size() > capacity_) records_.pop_front();

    if (!file_path_.empty() || queue_) {
        if (!queue_) {
            queue_ = new Queue();
            queue_->worker = std::thread([this] { writer_loop(); });
        }
        const std::string line = entry.to_json(true).dump(false) + "\n";
        {
            std::lock_guard<std::mutex> lock(queue_->mutex);
            // Cap the backlog so a stalled disk cannot grow memory without bound.
            if (queue_->pending.size() < 10000) queue_->pending.push_back(line);
        }
        queue_->cv.notify_one();
    }
    return entry.id;
}

void AuditLog::flush() {
    if (!queue_) return;
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(queue_->mutex);
            if (queue_->pending.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (file_handle_) ::FlushFileBuffers(static_cast<HANDLE>(file_handle_));
}

void AuditLog::stop() {
    if (!queue_) return;
    queue_->shutdown.store(true);
    queue_->cv.notify_all();
    if (queue_->worker.joinable()) queue_->worker.join();
    delete queue_;
    queue_ = nullptr;
}

std::vector<AuditRecord> AuditLog::query(const AuditQuery& query, bool newest_first) const {
    std::vector<AuditRecord> out;
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        const AuditRecord& record = *it;
        if (!query.document_id.empty() && record.document_id != query.document_id) continue;
        if (!query.session.empty() && record.session != query.session) continue;
        if (!query.tool.empty() && record.tool != query.tool) continue;
        if (query.source.has_value() && record.source != *query.source) continue;
        if (query.status.has_value() && record.status != *query.status) continue;
        if (query.since_ms > 0 && record.timestamp_ms < query.since_ms) continue;
        out.push_back(record);
        if (query.limit > 0 && out.size() >= query.limit) break;
    }
    if (!newest_first) std::reverse(out.begin(), out.end());
    return out;
}

const AuditRecord* AuditLog::find(int64_t id) const {
    for (const AuditRecord& record : records_) {
        if (record.id == id) return &record;
    }
    return nullptr;
}

void AuditLog::set_capacity(size_t records, size_t max_file_bytes) {
    capacity_ = std::max<size_t>(16, records);
    max_file_bytes_ = max_file_bytes;
    while (records_.size() > capacity_) records_.pop_front();
}

void AuditLog::clear() {
    records_.clear();
}

} // namespace colai
