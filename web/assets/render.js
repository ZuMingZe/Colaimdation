/* Colaimdation - Markdown parsing and rendering for the editor front-end.
 *
 * The parser mirrors src/core/markdown.cpp so that the block structure the
 * editor shows and the block ids the MCP tools address refer to the same
 * ranges. Every block carries its exact source span, which is what makes
 * "show the source only on the caret's line" and precise DOM<->source mapping
 * possible.
 */
(function (global) {
  'use strict';

  var MATH_PLACEHOLDER = '\u0000MATH';

  function escapeHtml(text) {
    return String(text)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function escapeAttr(text) {
    return escapeHtml(text).replace(/'/g, '&#39;');
  }

  // A markdown link destination is written straight into href/src. Escaping the
  // attribute is not enough: "javascript:..." is a perfectly valid attribute
  // value, and the page owns window.chrome.webview, so a document (or an agent
  // patch) containing [x](javascript:...) would run its code in the editor's own
  // origin on click. Only the schemes a Markdown editor legitimately needs are
  // allowed; anything else is rendered as plain text.
  function safeUrl(raw, allowImage) {
    var text = String(raw == null ? '' : raw).trim();
    if (!text) return '';
    // Strip control characters that browsers ignore when parsing the scheme.
    text = text.replace(/[\u0000-\u001F\u007F]/g, '');
    var scheme = /^([a-zA-Z][a-zA-Z0-9+.\-]*):/.exec(text);
    if (!scheme) return text; // relative path or fragment
    var name = scheme[1].toLowerCase();
    if (name === 'http' || name === 'https' || name === 'mailto') return text;
    if (allowImage && name === 'data') {
      return /^data:image\//i.test(text) ? text : '';
    }
    return '';
  }

  function tr(key, args) {
    return global.ColaiI18n ? global.ColaiI18n.t(key, args) : key;
  }

  function trimRight(text) {
    return text.replace(/[ \t]+$/, '');
  }

  function indentOf(line) {
    var n = 0;
    for (var i = 0; i < line.length; i++) {
      if (line[i] === ' ') n += 1;
      else if (line[i] === '\t') n += 4;
      else break;
    }
    return n;
  }

  function isBlank(line) {
    return /^[ \t]*$/.test(line);
  }

  function headingPrefix(line) {
    var m = /^ {0,3}(#{1,6})(?=[ \t]|$)/.exec(line);
    if (!m) return null;
    return { level: m[1].length, hashes: m[1].length };
  }

  function listMarker(line) {
    var m = /^([ \t]*)([-*+]|\d{1,9}[.)])([ \t]+|$)/.exec(line);
    if (!m) return null;
    return {
      indent: indentOf(line),
      marker: m[2],
      ordered: /\d/.test(m[2][0]),
      contentStart: m[1].length + m[2].length,
    };
  }

  function isThematicBreak(line) {
    var t = line.trim();
    if (t.length < 3) return false;
    return /^([-*_])(?:[ \t]*\1){2,}$/.test(t);
  }

  function isSetextUnderline(line) {
    var t = trimRight(line).trim();
    if (!t) return false;
    return /^(=+|-+)$/.test(t);
  }

  function isTableSeparator(line) {
    var t = line.trim();
    if (!t || t.indexOf('-') < 0) return false;
    return /^\|?[ \t]*:?-+:?[ \t]*(\|[ \t]*:?-+:?[ \t]*)*\|?$/.test(t);
  }

  function splitTableRow(line) {
    var text = line.trim();
    if (text.charAt(0) === '|') text = text.slice(1);
    if (text.charAt(text.length - 1) === '|') text = text.slice(0, -1);
    var cells = [];
    var current = '';
    for (var i = 0; i < text.length; i++) {
      var c = text[i];
      if (c === '\\' && i + 1 < text.length) { current += text[i + 1]; i++; continue; }
      if (c === '|') { cells.push(current.trim()); current = ''; continue; }
      current += c;
    }
    cells.push(current.trim());
    return cells;
  }

  function fenceInfo(line) {
    var m = /^[ \t]*(`{3,}|~{3,})(.*)$/.exec(line);
    if (!m) return null;
    var info = m[2].trim();
    var lang = info.split(/\s+/)[0] || '';
    return { fence: m[1], char: m[1][0], length: m[1].length, info: info, language: lang };
  }

  function blockStarter(lines, i) {
    // `lines` holds {text, start, end, next} records, so every helper below
    // needs the text, not the record. Passing the record made line.trim() throw
    // a TypeError the moment a paragraph was continued.
    var line = lines[i].text;
    if (isBlank(line)) return true;
    if (headingPrefix(line)) return true;
    if (isThematicBreak(line)) return true;
    if (listMarker(line)) return true;
    if (/^[ \t]*>/.test(line)) return true;
    if (fenceInfo(line)) return true;
    if (/^[ \t]*\$\$/.test(line)) return true;
    if (indentOf(line) >= 4) return true;
    if (i + 1 < lines.length && line.indexOf('|') >= 0 && isTableSeparator(lines[i + 1].text)) return true;
    return false;
  }

  /* ---------------------------------------------------------------------- */
  /* Block parsing                                                           */
  /* ---------------------------------------------------------------------- */
  function parseBlocks(text) {
    var source = String(text == null ? '' : text);
    var blocks = [];
    var offset = 0;
    var lines = [];
    var scan = 0;
    while (scan <= source.length) {
      var nl = source.indexOf('\n', scan);
      var end = nl === -1 ? source.length : nl;
      lines.push({ text: source.slice(scan, end), start: scan, end: end, next: nl === -1 ? source.length : nl + 1 });
      if (nl === -1) break;
      scan = nl + 1;
    }
    // A trailing empty line produced by a final newline is not a block.
    if (lines.length && lines[lines.length - 1].text === '' && lines[lines.length - 1].start === source.length) {
      lines.pop();
    }

    function push(kind, begin, end, contentBegin, contentEnd, extra) {
      var block = {
        kind: kind,
        start: begin,
        end: end,
        contentStart: contentBegin,
        contentEnd: contentEnd,
        source: source.slice(begin, end),
      };
      if (extra) for (var k in extra) if (Object.prototype.hasOwnProperty.call(extra, k)) block[k] = extra[k];
      blocks.push(block);
    }

    var i = 0;

    // Front matter
    if (lines.length && /^(---|\+\+\+)\s*$/.test(lines[0].text)) {
      var closing = lines[0].text.trim();
      for (var f = 1; f < Math.min(lines.length, 512); f++) {
        var candidate = lines[f].text.trim();
        if (candidate === closing || (closing === '---' && candidate === '...')) {
          push('front_matter', lines[0].start, lines[f].next, lines[0].next, lines[f].start, { info: closing });
          i = f + 1;
          break;
        }
      }
    }

    while (i < lines.length) {
      var line = lines[i].text;
      if (isBlank(line)) { i++; continue; }

      // Fenced code
      var fence = fenceInfo(line);
      if (fence) {
        var j = i + 1;
        for (; j < lines.length; j++) {
          var t = lines[j].text.trim();
          if (t.length >= fence.length) {
            var run = 0;
            while (run < t.length && t[run] === fence.char) run++;
            if (run >= fence.length && t.slice(run).trim() === '') break;
          }
        }
        var contentEnd = j < lines.length ? lines[j].start : source.length;
        var endOffset = j < lines.length ? lines[j].next : source.length;
        push('code_block', lines[i].start, endOffset, lines[i].next, contentEnd, {
          language: fence.language,
          info: fence.info,
          closed: j < lines.length,
        });
        i = j < lines.length ? j + 1 : lines.length;
        continue;
      }

      // Math block
      if (/^[ \t]*\$\$/.test(line) && line.indexOf('$$', line.indexOf('$$') + 2) === -1) {
        var m = i + 1;
        for (; m < lines.length; m++) if (lines[m].text.indexOf('$$') >= 0) break;
        var mEnd = m < lines.length ? lines[m].start : source.length;
        var mNext = m < lines.length ? lines[m].next : source.length;
        push('math_block', lines[i].start, mNext, lines[i].next, mEnd, {});
        i = m < lines.length ? m + 1 : lines.length;
        continue;
      }

      // ATX heading
      var heading = headingPrefix(line);
      if (heading) {
        var contentBegin = lines[i].start + line.indexOf('#'.repeat(heading.level)) + heading.level;
        while (contentBegin < lines[i].end && /[ \t]/.test(source[contentBegin])) contentBegin++;
        var content = trimRight(source.slice(contentBegin, lines[i].end));
        content = content.replace(/[ \t]+#+$/, '');
        push('heading', lines[i].start, lines[i].next, contentBegin, contentBegin + content.length, {
          level: heading.level,
          text: content,
        });
        i++;
        continue;
      }

      // Thematic break
      if (isThematicBreak(line)) {
        push('thematic_break', lines[i].start, lines[i].next, lines[i].start, lines[i].end, {});
        i++;
        continue;
      }

      // Block quote
      if (/^[ \t]*>/.test(line)) {
        var q = i;
        var qContentBegin = -1;
        var qContentEnd = lines[i].start;
        while (q < lines.length) {
          var qLine = lines[q].text;
          var lead = qLine.match(/^[ \t]*>[ \t]?/);
          if (!lead) {
            if (isBlank(qLine) && q + 1 < lines.length && /^[ \t]*>/.test(lines[q + 1].text)) { q++; continue; }
            break;
          }
          var cb = lines[q].start + lead[0].length;
          if (qContentBegin < 0) qContentBegin = cb;
          qContentEnd = lines[q].start + trimRight(qLine).length;
          q++;
        }
        push('block_quote', lines[i].start, lines[q - 1].next, qContentBegin < 0 ? lines[i].start : qContentBegin, Math.max(qContentEnd, qContentBegin), {});
        i = q;
        continue;
      }

      // Table
      if (i + 1 < lines.length && line.indexOf('|') >= 0 && isTableSeparator(lines[i + 1].text)) {
        var header = splitTableRow(line);
        var separators = splitTableRow(lines[i + 1].text);
        var aligns = separators.map(function (cell) {
          var c = cell.trim();
          var left = c.charAt(0) === ':';
          var right = c.charAt(c.length - 1) === ':';
          if (left && right) return 'center';
          if (left) return 'left';
          if (right) return 'right';
          return 'none';
        });
        var t2 = i + 2;
        var lastEnd = lines[i + 1].next;
        while (t2 < lines.length && !isBlank(lines[t2].text) && lines[t2].text.indexOf('|') >= 0) {
          lastEnd = lines[t2].next;
          t2++;
        }
        push('table', lines[i].start, lastEnd, lines[i].start, lastEnd, {
          header: header,
          alignments: aligns,
          rows: [],
        });
        // Parse body rows into the block payload.
        var tableBlock = blocks[blocks.length - 1];
        for (var r = i + 2; r < t2; r++) tableBlock.rows.push(splitTableRow(lines[r].text));
        i = t2;
        continue;
      }

      // List
      var opener = listMarker(line);
      if (opener) {
        var listStart = i;
        var l = i;
        var items = [];
        var maxDepth = 1;
        while (l < lines.length) {
          var cur = lines[l].text;
          if (isBlank(cur)) {
            var next = l + 1;
            while (next < lines.length && isBlank(lines[next].text)) next++;
            if (next < lines.length) {
              var nextMarker = listMarker(lines[next].text);
              if (nextMarker && nextMarker.ordered === opener.ordered) { l = next; continue; }
              if (indentOf(lines[next].text) >= 6) { l = next; continue; }
            }
            break;
          }
          var marker = listMarker(cur);
          if (marker) {
            var depth = Math.floor(marker.indent / 2) + 1;
            if (depth > maxDepth) maxDepth = depth;
            items.push({
              depth: depth,
              ordered: marker.ordered,
              marker: marker.marker,
              text: trimRight(cur.slice(marker.contentStart)).replace(/^[ \t]+/, ''),
              start: lines[l].start,
            });
          } else if (indentOf(cur) < 6) {
            break;
          }
          l++;
        }
        push('list', lines[listStart].start, lines[l - 1].next, lines[listStart].start, lines[l - 1].end, {
          ordered: opener.ordered,
          depth: maxDepth,
          items: items,
        });
        i = l;
        continue;
      }

      // Indented code
      if (indentOf(line) >= 4) {
        var c = i;
        var cEnd = lines[i].end;
        while (c < lines.length) {
          if (isBlank(lines[c].text)) { c++; continue; }
          if (indentOf(lines[c].text) < 4) break;
          cEnd = lines[c].end;
          c++;
        }
        var codeContent = lines.slice(i, c).map(function (entry) {
          return entry.text.replace(/^ {4}/, '').replace(/^\t/, '');
        }).join('\n');
        var block = {
          kind: 'code_block',
          start: lines[i].start,
          end: lines[c - 1].next,
          contentStart: lines[i].start + 4,
          contentEnd: cEnd,
          source: source.slice(lines[i].start, lines[c - 1].next),
          language: '',
          info: '',
          indented: true,
          code: codeContent,
        };
        blocks.push(block);
        i = c;
        continue;
      }

      // Paragraph (or setext heading)
      var p = i;
      var pEnd = lines[i].end;
      var setext = 0;
      while (p + 1 < lines.length) {
        var nextLine = lines[p + 1].text;
        if (isSetextUnderline(nextLine) && !isBlank(lines[p].text)) {
          setext = nextLine.trim().charAt(0) === '=' ? 1 : 2;
          pEnd = lines[p].end;
          p++;
          break;
        }
        if (blockStarter(lines, p + 1)) break;
        pEnd = lines[p + 1].end;
        p++;
      }
      if (setext) {
        push('heading', lines[i].start, lines[p].next, lines[i].start, pEnd, {
          level: setext,
          text: source.slice(lines[i].start, pEnd),
        });
      } else {
        push('paragraph', lines[i].start, lines[p].next, lines[i].start, pEnd, {});
      }
      i = p + 1;
    }

    for (var b = 0; b < blocks.length; b++) blocks[b].index = b;
    return blocks;
  }

  /* ---------------------------------------------------------------------- */
  /* Inline rendering                                                        */
  /* ---------------------------------------------------------------------- */
  function mathHtml(tex, displayMode) {
    var katex = global.katex;
    if (katex && typeof katex.renderToString === 'function') {
      try {
        return katex.renderToString(tex, { displayMode: !!displayMode, throwOnError: false, output: 'html' });
      } catch (error) {
        return '<span class="md-render-error">' + escapeHtml(String(error && error.message || error)) + '</span>';
      }
    }
    return '<code>' + escapeHtml(tex) + '</code>';
  }

  // Renders inline Markdown. Returns HTML with the original text preserved.
  function inlineHtml(text) {
    var out = '';
    var i = 0;
    var n = text.length;
    while (i < n) {
      var c = text.charAt(i);

      if (c === '\\' && i + 1 < n) { out += escapeHtml(text.substr(i, 2)); i += 2; continue; }

      // Inline code
      if (c === '`') {
        var run = 0;
        while (i + run < n && text.charAt(i + run) === '`') run++;
        var ticks = text.substr(i, run);
        var close = text.indexOf(ticks, i + run);
        if (close >= 0) {
          out += '<code>' + escapeHtml(text.slice(i + run, close)) + '</code>';
          i = close + run;
          continue;
        }
        out += escapeHtml(ticks);
        i += run;
        continue;
      }

      // Images and links
      if (c === '!' && text.charAt(i + 1) === '[') {
        var imgEnd = text.indexOf(']', i + 2);
        if (imgEnd >= 0 && text.charAt(imgEnd + 1) === '(') {
          var imgTarget = text.indexOf(')', imgEnd + 2);
          if (imgTarget >= 0) {
            var alt = text.slice(i + 2, imgEnd);
            var src = safeUrl(text.slice(imgEnd + 2, imgTarget).split(/\s+/)[0], true);
            if (!src) {
              // Unsafe destination: show the markup instead of an active tag.
              out += escapeHtml(text.slice(i, imgTarget + 1));
              i = imgTarget + 1;
              continue;
            }
            out += '<img src="' + escapeAttr(src) + '" alt="' + escapeAttr(alt) + '">';
            i = imgTarget + 1;
            continue;
          }
        }
      }
      if (c === '[') {
        var labelEnd = text.indexOf(']', i + 1);
        if (labelEnd >= 0 && text.charAt(labelEnd + 1) === '(') {
          var targetEnd = text.indexOf(')', labelEnd + 2);
          if (targetEnd >= 0) {
            var label = text.slice(i + 1, labelEnd);
            var target = safeUrl(text.slice(labelEnd + 2, targetEnd), false);
            if (!target) {
              out += escapeHtml(text.slice(i, targetEnd + 1));
              i = targetEnd + 1;
              continue;
            }
            out += '<a href="' + escapeAttr(target) + '" title="' + escapeAttr(target) +
              '" rel="noreferrer noopener">' + inlineHtml(label) + '</a>';
            i = targetEnd + 1;
            continue;
          }
        }
      }

      // Math
      if (c === '$') {
        var double = text.charAt(i + 1) === '$';
        var delim = double ? '$$' : '$';
        var mathEnd = text.indexOf(delim, i + delim.length);
        if (mathEnd > i + delim.length) {
          var tex = text.slice(i + delim.length, mathEnd);
          if (double || tex.charAt(0) !== ' ') {
            out += mathHtml(tex, double);
            i = mathEnd + delim.length;
            continue;
          }
        }
      }

      // Strong / emphasis / strikethrough
      if (c === '*' || c === '_' || c === '~') {
        var dbl = text.charAt(i + 1) === c;
        if (c === '~' && !dbl) { out += escapeHtml(c); i++; continue; }
        var mark = dbl ? c + c : c;
        var midEnd = text.indexOf(mark, i + mark.length);
        if (midEnd > i + mark.length) {
          var inner = inlineHtml(text.slice(i + mark.length, midEnd));
          if (c === '~') out += '<del>' + inner + '</del>';
          else if (dbl) out += '<strong>' + inner + '</strong>';
          else out += '<em>' + inner + '</em>';
          i = midEnd + mark.length;
          continue;
        }
      }

      // Autolink
      var rest = text.slice(i);
      var auto = /^<(https?:\/\/[^>\s]+)>/.exec(rest);
      if (auto) {
        out += '<a href="' + escapeAttr(auto[1]) + '">' + escapeHtml(auto[1]) + '</a>';
        i += auto[0].length;
        continue;
      }

      // Hard line break inside a paragraph
      if (c === '\n') { out += '<br>'; i++; continue; }

      out += escapeHtml(c);
      i++;
    }
    return out;
  }

  function headingHtml(block) {
    var level = block.level || 1;
    var text = block.text != null ? block.text : block.source.replace(/^#{1,6}\s*/, '').replace(/\s*#+\s*$/, '');
    return '<h' + level + '>' + inlineHtml(text) + '</h' + level + '>';
  }

  function codeHtml(block) {
    var code = block.code != null ? block.code : sourceContent(block);
    var language = (block.language || '').toLowerCase();
    return '<pre><code class="language-' + escapeAttr(language || 'none') + '">' +
      highlightCodeHtml(code, language) + '</code></pre>';
  }

  // Guessing the language of a snippet costs more than colouring one whose fence
  // names it, and the live code box calls this on every keystroke, so both bounds
  // are deliberately generous but finite.
  var kAutoHighlightLimit = 5000;
  var kHighlightLimit = 20000;

  // Highlighted HTML for code: the fence's language when highlight.js knows it, an
  // automatic guess when the fence has none, and escaped plain text when neither
  // applies. The editor's live code box (see editor.js _paintCodeHighlight) reuses
  // this, so the code being typed and the rendered block are coloured the same way.
  function highlightCodeHtml(code, language, allowAuto) {
    var text = String(code == null ? '' : code);
    var name = String(language || '').toLowerCase();
    var hljs = global.hljs;
    var highlighted = null;
    if (hljs && text.length <= kHighlightLimit) {
      if (name && hljs.getLanguage && hljs.getLanguage(name)) {
        try { highlighted = hljs.highlight(text, { language: name, ignoreIllegals: true }).value; }
        catch (error) { highlighted = null; }
      } else if (!name && allowAuto !== false && hljs.highlightAuto &&
                 text.length <= kAutoHighlightLimit) {
        try { highlighted = hljs.highlightAuto(text).value; }
        catch (error) { highlighted = null; }
      }
    }
    return highlighted !== null ? highlighted : escapeHtml(text);
  }

  function sourceContent(block) {
    var raw = block.source || '';
    // Strip the fence lines for fenced code blocks.
    if (block.kind === 'code_block' && !block.indented) {
      // The block's source runs to the start of the next block, so a fenced block
      // carries the newline that ends its closing fence line. Without dropping it
      // first, that closing fence is the *second to last* line here and stays in
      // the code - the rendered block showed a trailing "```" of its own.
      if (raw.charAt(raw.length - 1) === '\n') raw = raw.slice(0, -1);
      var lines = raw.split('\n');
      if (lines.length >= 1 && fenceInfo(lines[0])) lines.shift();
      if (lines.length && /^[ \t]*(`{3,}|~{3,})[ \t]*$/.test(lines[lines.length - 1])) lines.pop();
      return lines.join('\n');
    }
    return raw;
  }

  function renderList(items) {
    var html = '';
    for (var i = 0; i < items.length; i++) {
      var item = items[i];
      var task = /^\[([ xX])\]\s*/.exec(item.text);
      var content = task ? item.text.slice(task[0].length) : item.text;
      var checkbox = task
        ? '<input type="checkbox" disabled' + (task[1].toLowerCase() === 'x' ? ' checked' : '') + '> '
        : '';
      var position = i;
      html += '<li' + (task ? ' class="md-task"' : '') + '>' + checkbox + inlineHtml(content);
      var next = items[i + 1];
      if (next && next.depth > item.depth) {
        html += renderList(items.slice(i + 1).map(function (entry) {
          return { depth: entry.depth - (next.depth - item.depth), ordered: entry.ordered, text: entry.text };
        }));
        // Skip the nested entries we just consumed.
        var consumed = 1;
        for (var k = i + 1; k < items.length; k++) {
          if (items[k].depth <= item.depth) break;
          consumed++;
        }
        i += consumed - 1;
        html += '</li>';
        continue;
      }
      html += '</li>';
      void position;
    }
    var ordered = items.length ? items[0].ordered : false;
    return (ordered ? '<ol>' : '<ul>') + html + (ordered ? '</ol>' : '</ul>');
  }

  function listHtml(block) {
    var items = block.items || [];
    if (!items.length) return '<p>' + inlineHtml(block.source) + '</p>';
    return renderList(items);
  }

  function tableHtml(block) {
    var aligns = block.alignments || [];
    var rows = block.rows || [];
    var html = '<table><thead><tr>';
    (block.header || []).forEach(function (cell, index) {
      html += '<th' + alignClass(aligns[index]) + '>' + inlineHtml(cell) + '</th>';
    });
    html += '</tr></thead><tbody>';
    rows.forEach(function (row) {
      html += '<tr>';
      for (var i = 0; i < (block.header || []).length; i++) {
        html += '<td' + alignClass(aligns[i]) + '>' + inlineHtml(row[i] == null ? '' : row[i]) + '</td>';
      }
      html += '</tr>';
    });
    html += '</tbody></table>';
    return html;
  }

  function alignClass(align) {
    if (align === 'center') return ' class="md-align-center"';
    if (align === 'right') return ' class="md-align-right"';
    return '';
  }

  function blockHtml(block) {
    switch (block.kind) {
      case 'front_matter':
        return '<div class="md-frontmatter">' + escapeHtml(block.source.replace(/^(---|\+\+\+)\n?/, '').replace(/\n?(---|\+\+\+|\.\.\.)\s*$/, '')) + '</div>';
      case 'heading':
        return headingHtml(block);
      case 'code_block':
        if ((block.language || '').toLowerCase() === 'mermaid') {
          return '<div class="md-mermaid" data-mermaid="' + escapeAttr(sourceContent(block)) + '"></div>';
        }
        return codeHtml(block);
      case 'math_block': {
        // A block's source keeps the newline that terminates its closing "$$" line,
        // so the closing delimiter is NOT at the end of the string. The old pattern
        // only matched when the formula happened to be the last thing in the file;
        // everywhere else KaTeX was handed a stray "$$" and drew an error instead
        // of the formula (which reads as "the formula does not render").
        var tex = block.source
          .replace(/^[ \t]*\$\$[ \t]*\r?\n?/, '')
          .replace(/\r?\n?[ \t]*\$\$[ \t]*\r?\n?$/, '');
        return '<div class="md-math-block">' + mathHtml(tex.replace(/^\s+|\s+$/g, ''), true) + '</div>';
      }
      case 'table':
        return tableHtml(block);
      case 'list':
        return listHtml(block);
      case 'block_quote':
        return '<blockquote>' + block.source.replace(/^[ \t]*>[ \t]?/gm, '').split(/\n{2,}/).map(function (part) {
          return '<p>' + inlineHtml(part.replace(/\n$/, '')) + '</p>';
        }).join('') + '</blockquote>';
      case 'thematic_break':
        return '<hr>';
      default:
        return '<p>' + inlineHtml(block.source.replace(/\n$/, '')) + '</p>';
    }
  }

  /* ---------------------------------------------------------------------- */
  /* Mermaid                                                                 */
  /* ---------------------------------------------------------------------- */
  var mermaidReady = false;

  function initMermaid() {
    if (mermaidReady || !global.mermaid) return;
    try {
      global.mermaid.initialize({
        startOnLoad: false,
        securityLevel: 'strict',
        theme: document.documentElement.getAttribute('data-theme') === 'dark' ? 'dark' : 'default',
        fontFamily: getComputedStyle(document.body).fontFamily,
      });
      mermaidReady = true;
    } catch (error) {
      /* Mermaid is optional: fall back to showing the source. */
    }
  }

  var mermaidCounter = 0;

  function renderMermaid(root) {
    if (!global.mermaid) return;
    initMermaid();
    var nodes = root.querySelectorAll('.md-mermaid[data-mermaid]');
    Array.prototype.forEach.call(nodes, function (node) {
      var code = node.getAttribute('data-mermaid');
      if (!code) return;
      node.removeAttribute('data-mermaid');
      var id = 'mmd-' + (++mermaidCounter);
      var failure = function (error) {
        node.innerHTML = '<div class="md-render-error">' +
          escapeHtml(tr('editor.mermaidError', [String(error && error.message || error)])) +
          '</div>';
      };
      try {
        var result = global.mermaid.render(id, code);
        if (result && typeof result.then === 'function') {
          result.then(function (out) {
            node.innerHTML = out.svg || out;
          }).catch(failure);
        } else if (result && result.svg) {
          node.innerHTML = result.svg;
        }
      } catch (error) {
        failure(error);
      }
    });
  }

  function refreshMermaidTheme() {
    if (!global.mermaid) return;
    try {
      global.mermaid.initialize({
        startOnLoad: false,
        securityLevel: 'strict',
        theme: document.documentElement.getAttribute('data-theme') === 'dark' ? 'dark' : 'default',
      });
    } catch (error) { /* ignore */ }
  }

  global.ColaiRender = {
    parseBlocks: parseBlocks,
    blockHtml: blockHtml,
    inlineHtml: inlineHtml,
    sourceContent: sourceContent,
    highlightCodeHtml: highlightCodeHtml,
    renderMermaid: renderMermaid,
    refreshMermaidTheme: refreshMermaidTheme,
    escapeHtml: escapeHtml,
    mathHtml: mathHtml,
    isBlank: isBlank,
    listMarker: listMarker,
    headingPrefix: headingPrefix,
    fenceInfo: fenceInfo,
  };
})(window);
