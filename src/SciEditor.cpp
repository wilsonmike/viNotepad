#include "SciEditor.h"

#include <algorithm>
#include <vector>

int sciInsertCaretStyle = CARETSTYLE_LINE;

std::wstring toWide(const std::string& s, UINT cp)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

std::string fromWide(const std::wstring& w, UINT cp)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

SciEditor::SciEditor(HWND h) : hwnd(h)
{
	fn = (SciFnDirect)SendMessage(h, SCI_GETDIRECTFUNCTION, 0, 0);
	ptr = (sptr_t)SendMessage(h, SCI_GETDIRECTPOINTER, 0, 0);
}

UINT SciEditor::codePage()
{
	int cp = (int)S(SCI_GETCODEPAGE);
	return cp == 0 ? CP_ACP : (UINT)cp;
}

intptr_t SciEditor::docId() { return (intptr_t)S(SCI_GETDOCPOINTER); }
Pos SciEditor::length() { return (Pos)S(SCI_GETLENGTH); }
int SciEditor::byteAt(Pos p) { return p >= 0 && p < length() ? (int)(S(SCI_GETCHARAT, (uptr_t)p) & 0xFF) : 0; }
Pos SciEditor::after(Pos p) { return (Pos)S(SCI_POSITIONAFTER, (uptr_t)p); }
Pos SciEditor::before(Pos p) { return (Pos)S(SCI_POSITIONBEFORE, (uptr_t)p); }
Pos SciEditor::lineCount() { return (Pos)S(SCI_GETLINECOUNT); }
Pos SciEditor::lineOf(Pos p) { return (Pos)S(SCI_LINEFROMPOSITION, (uptr_t)p); }

Pos SciEditor::lineStart(Pos line)
{
	if (line < 0) return 0;
	if (line >= lineCount()) return length();
	return (Pos)S(SCI_POSITIONFROMLINE, (uptr_t)line);
}

Pos SciEditor::lineEnd(Pos line)
{
	if (line < 0) line = 0;
	if (line >= lineCount()) return length();
	return (Pos)S(SCI_GETLINEENDPOSITION, (uptr_t)line);
}

Pos SciEditor::column(Pos p) { return (Pos)S(SCI_GETCOLUMN, (uptr_t)p); }
Pos SciEditor::posAtColumn(Pos line, Pos col) { return (Pos)S(SCI_FINDCOLUMN, (uptr_t)line, (sptr_t)col); }

std::string SciEditor::text(Pos a, Pos b)
{
	Pos len = length();
	a = (std::max)((Pos)0, (std::min)(a, len));
	b = (std::max)(a, (std::min)(b, len));
	if (b == a) return std::string();
	std::vector<char> buf((size_t)(b - a + 1));
	Sci_TextRangeFull tr;
	tr.chrg.cpMin = a;
	tr.chrg.cpMax = b;
	tr.lpstrText = buf.data();
	S(SCI_GETTEXTRANGEFULL, 0, (sptr_t)&tr);
	return std::string(buf.data(), (size_t)(b - a));
}

void SciEditor::insert(Pos at, const std::string& s)
{
	if (s.empty()) return;
	S(SCI_SETTARGETRANGE, (uptr_t)at, (sptr_t)at);
	S(SCI_REPLACETARGET, (uptr_t)s.size(), (sptr_t)s.data());
}

void SciEditor::remove(Pos a, Pos b)
{
	if (b > a) S(SCI_DELETERANGE, (uptr_t)a, (sptr_t)(b - a));
}

Pos SciEditor::caret() { return (Pos)S(SCI_GETCURRENTPOS); }
Pos SciEditor::anchor() { return (Pos)S(SCI_GETANCHOR); }

void SciEditor::select(Pos a, Pos c)
{
	S(SCI_SETSEL, (uptr_t)a, (sptr_t)c);
	S(SCI_CHOOSECARETX);
}

void SciEditor::selectBlock(Pos aLine, Pos aCol, Pos cLine, Pos cCol)
{
	Pos ap = posAtColumn(aLine, aCol), cp = posAtColumn(cLine, cCol);
	Pos av = (std::max)((Pos)0, aCol - column(ap)), cv = (std::max)((Pos)0, cCol - column(cp));
	S(SCI_SETRECTANGULARSELECTIONANCHOR, (uptr_t)ap);
	S(SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE, (uptr_t)av);
	S(SCI_SETRECTANGULARSELECTIONCARET, (uptr_t)cp);
	S(SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE, (uptr_t)cv);
	S(SCI_SCROLLCARET);
}

void SciEditor::beginUndo() { S(SCI_BEGINUNDOACTION); }
void SciEditor::endUndo() { S(SCI_ENDUNDOACTION); }
void SciEditor::undo() { S(SCI_UNDO); }
void SciEditor::redo() { S(SCI_REDO); }

std::string SciEditor::eol()
{
	switch (S(SCI_GETEOLMODE)) {
	case SC_EOL_CRLF: return "\r\n";
	case SC_EOL_CR: return "\r";
	default: return "\n";
	}
}

int SciEditor::indentWidth()
{
	int w = (int)S(SCI_GETINDENT);
	return w > 0 ? w : tabWidth();
}

int SciEditor::tabWidth()
{
	int w = (int)S(SCI_GETTABWIDTH);
	return w > 0 ? w : 4;
}

bool SciEditor::useTabs() { return S(SCI_GETUSETABS) != 0; }
Pos SciEditor::lineIndent(Pos line) { return (Pos)S(SCI_GETLINEINDENTATION, (uptr_t)line); }
void SciEditor::setLineIndent(Pos line, Pos width) { S(SCI_SETLINEINDENTATION, (uptr_t)line, (sptr_t)width); }

bool SciEditor::find(const std::string& re, Pos from, Pos to, bool icase, Pos& s, Pos& e)
{
	S(SCI_SETTARGETRANGE, (uptr_t)from, (sptr_t)to);
	S(SCI_SETSEARCHFLAGS, (uptr_t)(SCFIND_REGEXP | SCFIND_POSIX | 0x40000000u | 0x80000000u | (icase ? 0 : SCFIND_MATCHCASE)));
	Pos r = (Pos)S(SCI_SEARCHINTARGET, (uptr_t)re.size(), (sptr_t)re.data());
	if (r < 0) return false;
	s = r;
	e = (Pos)S(SCI_GETTARGETEND);
	matchS = s;
	matchE = e;
	return true;
}

std::string SciEditor::group(int n)
{
	if (n == 0) return text(matchS, matchE);
	sptr_t len = S(SCI_GETTAG, (uptr_t)n, 0);
	if (len <= 0) return std::string();
	std::vector<char> buf((size_t)len + 1);
	S(SCI_GETTAG, (uptr_t)n, (sptr_t)buf.data());
	return std::string(buf.data(), (size_t)len);
}

Pos SciEditor::topLine() { return (Pos)S(SCI_DOCLINEFROMVISIBLE, (uptr_t)S(SCI_GETFIRSTVISIBLELINE)); }
void SciEditor::setTopLine(Pos line) { S(SCI_SETFIRSTVISIBLELINE, (uptr_t)S(SCI_VISIBLEFROMDOCLINE, (uptr_t)(std::max)((Pos)0, line))); }
Pos SciEditor::visibleLines() { return (Pos)S(SCI_LINESONSCREEN); }

std::string SciEditor::encode(char32_t c)
{
	std::wstring w;
	if (c >= 0x10000) {
		c -= 0x10000;
		w += (wchar_t)(0xD800 + (c >> 10));
		w += (wchar_t)(0xDC00 + (c & 0x3FF));
	} else w += (wchar_t)c;
	return fromWide(w, codePage());
}

Keys SciEditor::decode(const std::string& s)
{
	std::wstring w = toWide(s, codePage());
	Keys k;
	for (size_t i = 0; i < w.size(); i++) {
		char32_t c = w[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
		k += c;
	}
	return k;
}

std::string SciEditor::mapCase(const std::string& s, CaseMode m)
{
	UINT cp = codePage();
	std::wstring w = toWide(s, cp);
	if (w.empty()) return s;
	if (m == CASE_UPPER) CharUpperBuffW(&w[0], (DWORD)w.size());
	else if (m == CASE_LOWER) CharLowerBuffW(&w[0], (DWORD)w.size());
	else for (wchar_t& ch : w) {
		if (IsCharUpperW(ch)) CharLowerBuffW(&ch, 1);
		else if (IsCharLowerW(ch)) CharUpperBuffW(&ch, 1);
	}
	return fromWide(w, cp);
}

std::string SciEditor::clipboard()
{
	std::string out;
	if (!OpenClipboard(hwnd)) return out;
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (h) {
		const wchar_t* p = (const wchar_t*)GlobalLock(h);
		if (p) { out = fromWide(std::wstring(p), codePage()); GlobalUnlock(h); }
	}
	CloseClipboard();
	return out;
}

void SciEditor::setClipboard(const std::string& s)
{
	std::wstring w = toWide(s, codePage());
	if (!OpenClipboard(hwnd)) return;
	EmptyClipboard();
	HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
	if (g) {
		wchar_t* p = (wchar_t*)GlobalLock(g);
		if (p) {
			memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
			GlobalUnlock(g);
			if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
		} else GlobalFree(g);
	}
	CloseClipboard();
}

void SciEditor::setCaretStyle(bool block) { S(SCI_SETCARETSTYLE, (uptr_t)(block ? CARETSTYLE_BLOCK : sciInsertCaretStyle)); }
void SciEditor::setOvertype(bool on) { S(SCI_SETOVERTYPE, on ? 1 : 0); }

void SciEditor::cancelPopups()
{
	S(SCI_AUTOCCANCEL);
	S(SCI_CALLTIPCANCEL);
}
