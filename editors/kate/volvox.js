var katescript = {
    "name": "Volvox",
    "author": "Uwe Krüger <uwe_debbug@arcor.de>",
    "license": "BSD",
    "revision": 1,
    "kate-version": "5.1"
}; // kate-script-header, must be at the start of the file without comments, pure json

// Volvox indenter

// required katepart js libraries
require ("range.js");

var BlockStart = /\s*([\(\{\[]|(def|cdef|if|while|for|lock|repeat|loop)\b)/;
var BlockElse = /\s*(else|elif|brk)\b/;
// TODO: we should better use a loop here, but >4 brk in one line are rare
var BlockElse2 = /\s*brk  *brk\b/;
var BlockElse3 = /\s*brk  *brk  *brk\b/;
var BlockElse4 = /\s*brk  *brk  *brk  *brk\b/;
var BlockEnd = /([\)\]\}]|\bend\b)/;
var BlockEnd2 = /([\)\]\}]  *[\)\]\}]|\bend  *end\b)/;
var BlockEnd3 = /([\)\]\}]  *[\)\]\}]  *[\)\]\}]|\bend  *end  *end\b)/;
var BlockEnd4 = /([\)\]\}]  *[\)\]\}]  *[\)\]\}]  *[\)\]\}]|\bend  *end  *end  *end\b)/;
var BlockEnd5 = /([\)\]\}]  *[\)\]\}]  *[\)\]\}]  *[\)\]\}]  *[\)\]\}]|\bend  *end  *end  *end  *end\b)/;
var indent_width = 4;

triggerCharacters = "{}[]():;#";

function indent(line, indentWidth, ch)
{
	var oldline = line;
	if (ch == '\n') {
		oldline--;
	}
	var firstCol = document.firstColumn(oldline);
	if (firstCol < 0) {
		// remove whitespaces from empty lines
		if (ch == '\n') {
			document.truncate(oldline, 0);
		} else {
			return 0;
		}
	}
	var prevLine = document.prevNonEmptyLine(line-1);
	var lastindent = document.toVirtualColumn(prevLine, document.firstColumn(prevLine));
	var elseLine = line;
	if (ch == '\n') {
		elseLine = prevLine;
	}
	// check for else/elif and unindent if necessary
	if (BlockElse.test(document.line(elseLine)) || BlockEnd.test(document.line(elseLine))) {
		var b4line = document.prevNonEmptyLine(elseLine-1);
		var elseIndent = document.toVirtualColumn(b4line, document.firstColumn(b4line));
		if (!(BlockStart.test(document.line(b4line)))) {
			elseIndent -= indent_width;
			if (BlockElse2.test(document.line(elseLine))) {
				elseIndent -= indent_width;
				if (BlockElse3.test(document.line(elseLine))) {
					elseIndent -= indent_width;
					if (BlockElse4.test(document.line(elseLine))) {
						elseIndent -= indent_width;
					}
				}
			} else if (BlockEnd2.test(document.line(elseLine))) {
				elseIndent -= indent_width;
				if (BlockEnd3.test(document.line(elseLine))) {
					elseIndent -= indent_width;
					if (BlockEnd4.test(document.line(elseLine))) {
						elseIndent -= indent_width;
						if (BlockEnd5.test(document.line(elseLine))) {
							elseIndent -= indent_width;
						}
					}
				}
			}
		}
		if (ch == '\n') {
			var curIndent = document.toVirtualColumn(elseLine, document.firstColumn(elseLine)) / indent_width;
			var newIndent = elseIndent / indent_width;
			document.indent(new Range(elseLine, 0, elseLine, 1), newIndent - curIndent);
			lastindent = elseIndent;
		} else {
			return elseIndent;
		}
	}
	if (BlockStart.test(document.line(prevLine)) || BlockElse.test(document.line(prevLine))) {
		if (BlockElse2.test(document.line(prevLine))) {
			if (BlockElse3.test(document.line(prevLine))) {
				if (BlockElse4.test(document.line(prevLine))) {
					return lastindent + 4*indent_width;
				}
				return lastindent + 3*indent_width;
			}
			return lastindent + 2*indent_width;
		}
		return lastindent + indent_width;
	}
		
	return -1; // indent as in previous non-empty line
}

// kate: space-indent on; indent-width 4; replace-tabs off;
