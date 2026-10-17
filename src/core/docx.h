// Colaimdation - exporting a document as Word (.docx).
//
// The document is already parsed here (see core/markdown.h), so the export walks the
// same block model the editor renders instead of re-parsing Markdown with a second
// implementation. The package is assembled with core/zip_writer.h; nothing outside the
// standard library is involved, which keeps the "no third-party runtime dependency"
// promise of the project intact.
//
// What is mapped: headings, paragraphs with bold/italic/strikethrough/inline code,
// links (as real Word hyperlinks), bullet/numbered/task lists, block quotes, fenced
// and indented code, tables (with their alignment), and horizontal rules. Formulas are
// written as their LaTeX source and images as a placeholder line: converting either
// would mean shipping a formula layout engine or an image decoder, and a wrong
// rendering is worse than an honest one.
#pragma once

#include <string>
#include <string_view>

namespace colai::docx {

// The whole package, ready to be written to a .docx path.
std::string from_markdown(std::string_view markdown, std::string_view title);

// The `word/document.xml` part on its own. Exposed for the tests, which check the
// structure the export produces without unpacking a package.
std::string document_xml(std::string_view markdown);

} // namespace colai::docx
