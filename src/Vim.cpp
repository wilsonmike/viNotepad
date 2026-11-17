#include "Vim.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

static bool isDigit(char32_t c) { return c >= '0' && c <= '9'; }
static bool isLower(char32_t c) { return c >= 'a' && c <= 'z'; }
static bool isUpper(char32_t c) { return c >= 'A' && c <= 'Z'; }
static const Pos INF = INTPTR_MAX / 4;

static std::string utf8(const Keys& k)
{
	std::string s;
	for (char32_t c : k) {
		if (c < 0x80) s += (char)c;
		else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
		else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
	}
	return s;
}

static Keys fromUtf8(const std::string& s)
{
	Keys k;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		char32_t v = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
		i++;
		for (int j = 0; j < n && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80; j++, i++) v = (v << 6) | ((unsigned char)s[i] & 0x3F);
		k += v;
	}
	return k;
}

static Keys keysOf(const char* s)
{
	Keys k;
	while (*s) k += (char32_t)(unsigned char)*s++;
	return k;
}

static std::string trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void Vim::enter(Editor& e)
{
	ed = &e;
	md = NORMAL;
	pend.clear();
	e.setOvertype(false);
	e.setCaretStyle(true);
	Pos c = clampNormal(e.caret());
	if (e.anchor() == e.caret()) moveTo(c);
	syncSel();
	host.status(statusText());
}

void Vim::leave(Editor& e)
{
	ed = &e;
	settleUndo();
	e.setOvertype(false);
	e.setCaretStyle(false);
	md = NORMAL;
	pend.clear();
}

void Vim::bufferActivated(Editor& e)
{
	ed = &e;
	settleUndo();
	e.setOvertype(false);
	md = NORMAL;
	pend.clear();
	insPending = false;
	blockIns = false;
	e.setCaretStyle(true);
	if (e.anchor() == e.caret()) moveTo(clampNormal(e.caret()));
	syncSel();
	host.status(statusText());
}

void Vim::textChanged(Editor& e, Pos at, Pos len, bool inserted)
{
	auto it = marks.find(e.docId());
	if (it == marks.end() || len <= 0) return;
	for (auto& kv : it->second) {
		Pos& p = kv.second;
		if (inserted) { if (p >= at) p += len; }
		else if (p >= at + len) p -= len;
		else if (p > at) p = at;
	}
}

void Vim::selectionChanged(Editor& e)
{
	ed = &e;
	if (insertLike() || md == CMDLINE) return;
	Pos a = e.anchor(), c = e.caret();
	if (a == selA && c == selC) return;
	if (a == c) {
		if (md == VISUAL || md == VLINE || md == VBLOCK) setMode(NORMAL);
		pend.clear();
		Pos p = clampNormal(c);
		if (p != c) e.select(p, p);
		want = e.column(p);
	} else {
		if (md != VISUAL) { pend.clear(); setMode(VISUAL); }
		vAnchor = a;
		vCur = c > a ? e.before(c) : c;
		want = e.column(vCur);
	}
	syncSel();
	host.status(statusText());
}

void Vim::syncSel()
{
	selA = ed->anchor();
	selC = ed->caret();
}

std::string Vim::statusText() const
{
	std::string s;
	switch (md) {
	case NORMAL: s = "NORMAL"; break;
	case INSERT: s = blockIns ? "INSERT (block)" : "INSERT"; break;
	case REPLACE: s = "REPLACE"; break;
	case VISUAL: s = "VISUAL"; break;
	case VLINE: s = "VISUAL LINE"; break;
	case VBLOCK: s = "VISUAL BLOCK"; break;
	case CMDLINE: return std::string(1, (char)cmdType) + cmdline + "_";
	}
	s = "-- " + s + " --";
	if (!pend.empty()) {
		Keys shown;
		for (char32_t c : pend) {
			if (c < 32) { shown += '^'; shown += (char32_t)(c + '@'); }
			else if (c < 0xE000 || c > 0xF8FF) shown += c;
		}
		s += "   " + utf8(shown);
	}
	if (recReg) s += "   recording @" + utf8(Keys(1, recReg));
	if (!msg.empty()) s += "   " + msg;
	return s;
}

void Vim::key(Editor& e, char32_t k)
{
	ed = &e;
	msg.clear();
	if (recReg) recBuf += k;
	feed(k);
	syncSel();
	host.status(statusText());
}

void Vim::observe(Editor& e, char32_t k)
{
	ed = &e;
	if (!(k >= 32 || k == K_BS || k == K_TAB || k == K_ENTER)) return;
	if (recReg) recBuf += k;
	insKeys += k;
}

void Vim::feed(char32_t k)
{
	if (insertLike()) insertKey(k);
	else if (md == CMDLINE) cmdlineKey(k);
	else normalKey(k);
}

void Vim::setMode(Mode m)
{
	md = m;
	if (m != CMDLINE) ed->setCaretStyle(m != INSERT && m != REPLACE);
}

Pos Vim::curLine() { return ed->lineOf(md == VISUAL || md == VLINE || md == VBLOCK ? vCur : ed->caret()); }

Pos Vim::clampNormal(Pos p)
{
	Pos line = ed->lineOf(p);
	Pos s = ed->lineStart(line), e = ed->lineEnd(line);
	if (p >= e && e > s) return ed->before(e);
	if (p > e) return e;
	return p;
}

void Vim::moveTo(Pos p, bool keepWant)
{
	if (md == VISUAL || md == VLINE || md == VBLOCK) {
		Pos line = ed->lineOf(p);
		if (p >= ed->lineEnd(line) && ed->lineEnd(line) > ed->lineStart(line) && md != VBLOCK) p = ed->before(ed->lineEnd(line));
		vCur = p;
		if (!keepWant) want = ed->column(p);
		renderVisual();
		return;
	}
	if (md == NORMAL) p = clampNormal(p);
	ed->select(p, p);
	if (!keepWant) want = ed->column(p);
}

Pos Vim::firstNonBlank(Pos line)
{
	Pos p = ed->lineStart(line), e = ed->lineEnd(line);
	while (p < e && (ed->byteAt(p) == ' ' || ed->byteAt(p) == '\t')) p++;
	return p;
}

Pos Vim::lineLastChar(Pos line)
{
	Pos s = ed->lineStart(line), e = ed->lineEnd(line);
	return e > s ? ed->before(e) : s;
}

bool Vim::isBlankLine(Pos line) { return ed->lineStart(line) == ed->lineEnd(line); }

std::string Vim::indentOf(Pos line)
{
	return ed->text(ed->lineStart(line), firstNonBlank(line));
}

int Vim::cls(Pos p, bool big)
{
	if (p >= ed->length()) return 0;
	int b = ed->byteAt(p);
	if (b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == 0) return 0;
	if (big) return 1;
	if ((b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_' || b >= 0x80) return 1;
	return 2;
}

Pos Vim::wordFwd(Pos p, bool big, bool stopEol)
{
	Pos len = ed->length();
	int c = cls(p, big);
	Pos line = ed->lineOf(p);
	if (c != 0) while (p < len && cls(p, big) == c) p = ed->after(p);
	while (p < len) {
		int b = ed->byteAt(p);
		if (b == '\r' || b == '\n') {
			if (stopEol) return p;
			p = ed->after(p);
			Pos l = ed->lineOf(p);
			if (l != line && isBlankLine(l)) return p;
			continue;
		}
		if (b == ' ' || b == '\t') { p = ed->after(p); continue; }
		break;
	}
	return p;
}

Pos Vim::wordEnd(Pos p, bool big)
{
	Pos len = ed->length();
	p = ed->after(p);
	while (p < len && cls(p, big) == 0) p = ed->after(p);
	if (p >= len) return ed->before(len);
	int c = cls(p, big);
	while (ed->after(p) < len && cls(ed->after(p), big) == c) p = ed->after(p);
	return p;
}

Pos Vim::wordBack(Pos p, bool big)
{
	if (p <= 0) return 0;
	Pos startLine = ed->lineOf(p);
	p = ed->before(p);
	while (p > 0 && cls(p, big) == 0) {
		Pos l = ed->lineOf(p);
		if (l != startLine && isBlankLine(l)) return p;
		p = ed->before(p);
	}
	int c = cls(p, big);
	while (p > 0 && cls(ed->before(p), big) == c && c != 0) p = ed->before(p);
	return p;
}

Pos Vim::wordEndBack(Pos p, bool big)
{
	Pos startLine = ed->lineOf(p);
	int c = cls(p, big);
	if (c != 0) while (p > 0 && cls(p, big) == c) p = ed->before(p);
	while (p > 0 && cls(p, big) == 0) {
		Pos l = ed->lineOf(p);
		if (l != startLine && isBlankLine(l)) return p;
		p = ed->before(p);
	}
	return p;
}

Pos Vim::findChar(Pos p, char32_t cmd, char32_t ch, int count, bool repeat)
{
	std::string s = ed->encode(ch);
	Pos line = ed->lineOf(p), ls = ed->lineStart(line), le = ed->lineEnd(line);
	bool fwd = cmd == 'f' || cmd == 't';
	Pos q = p;
	if (repeat && (cmd == 't' || cmd == 'T')) q = fwd ? ed->after(q) : ed->before(q);
	for (int i = 0; i < count; i++) {
		for (;;) {
			if (fwd) { if (q >= le) return -1; q = ed->after(q); if (q >= le) return -1; }
			else { if (q <= ls) return -1; q = ed->before(q); }
			if (ed->text(q, q + (Pos)s.size()) == s) break;
		}
	}
	if (cmd == 't') q = ed->before(q);
	if (cmd == 'T') q = ed->after(q);
	return q;
}

Pos Vim::matchPair(Pos p)
{
	static const char* pairs = "()[]{}";
	Pos le = ed->lineEnd(ed->lineOf(p));
	while (p < le && !strchr(pairs, ed->byteAt(p))) p++;
	if (p >= le) return -1;
	int b = ed->byteAt(p);
	const char* at = strchr(pairs, b);
	int idx = (int)(at - pairs);
	int open = pairs[idx & ~1], close = pairs[idx | 1];
	int dir = (idx & 1) ? -1 : 1;
	int nest = 0;
	Pos len = ed->length();
	for (Pos q = p; q >= 0 && q < len; q += dir) {
		int c = ed->byteAt(q);
		if (c == open) nest += dir;
		else if (c == close) nest -= dir;
		if (nest == 0) return q;
	}
	return -1;
}

Pos Vim::paragraph(Pos p, int dir, int count)
{
	Pos line = ed->lineOf(p), n = ed->lineCount();
	for (int i = 0; i < count; i++) {
		while (line + dir >= 0 && line + dir < n && isBlankLine(line)) line += dir;
		while (line + dir >= 0 && line + dir < n && !isBlankLine(line)) line += dir;
		if (!isBlankLine(line)) return dir > 0 ? ed->lineEnd(line) : ed->lineStart(line);
	}
	return ed->lineStart(line);
}

std::string Vim::wordUnder(Pos p, Pos& s)
{
	Pos le = ed->lineEnd(ed->lineOf(p));
	while (p < le && cls(p, false) != 1) p = ed->after(p);
	if (p >= le) return std::string();
	s = p;
	while (s > ed->lineStart(ed->lineOf(p)) && cls(ed->before(s), false) == 1) s = ed->before(s);
	Pos e = p;
	while (e < le && cls(e, false) == 1) e = ed->after(e);
	return ed->text(s, e);
}

Vim::Parse Vim::parseMotion(const Keys& k, size_t& i, Cmd& c, bool allowObj)
{
	if (i >= k.size()) return MORE;
	char32_t a = k[i++];
	static const Keys single = keysOf("hjklwWbBeE0^$G{}%HMLnN*#;,+-_| ") + Keys{ K_ENTER, K_BS };
	if (single.find(a) != Keys::npos) { c.mot = Keys(1, a); return OK; }
	if (a == 'f' || a == 'F' || a == 't' || a == 'T' || a == '\'' || a == '`') {
		if (i >= k.size()) return MORE;
		c.mot = Keys(1, a);
		c.arg = k[i++];
		return c.arg == K_ESC ? BAD : OK;
	}
	if (a == 'g') {
		if (i >= k.size()) return MORE;
		char32_t b = k[i++];
		if (Keys(keysOf("geE_jk0$m")).find(b) != Keys::npos) { c.mot = Keys{ 'g', b }; return OK; }
		return NONE;
	}
	if (allowObj && (a == 'i' || a == 'a')) {
		if (i >= k.size()) return MORE;
		char32_t b = k[i++];
		if (Keys(keysOf("wWp\"'`()bB{}[]<>")).find(b) != Keys::npos) { c.mot = Keys{ a, b }; return OK; }
		return BAD;
	}
	return NONE;
}

static size_t readCount(const Keys& k, size_t i, int& n)
{
	if (i < k.size() && k[i] >= '1' && k[i] <= '9') {
		long long v = 0;
		while (i < k.size() && isDigit(k[i])) { v = std::min<long long>(v * 10 + (k[i] - '0'), 999999); i++; }
		n = (int)v;
	}
	return i;
}

Vim::Parse Vim::parse(const Keys& k, Cmd& c, bool visual)
{
	size_t i = 0;
	if (i < k.size() && k[i] == '"') {
		i++;
		if (i >= k.size()) return MORE;
		c.reg = k[i++];
		static const Keys ok = keysOf("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\"-+*_/.");
		if (ok.find(c.reg) == Keys::npos) return BAD;
	}
	i = readCount(k, i, c.count);
	if (i >= k.size()) return MORE;
	c.body = i;
	char32_t a = k[i];
	Keys op;
	if (a == 'd' || a == 'c' || a == 'y' || a == '>' || a == '<' || a == '=') op = Keys(1, a);
	else if (a == 'g' && i + 1 < k.size() && (k[i + 1] == '~' || k[i + 1] == 'u' || k[i + 1] == 'U')) op = Keys{ 'g', k[i + 1] };
	else if (a == 'g' && i + 1 >= k.size()) return MORE;
	if (!op.empty()) {
		c.op = op;
		if (visual) return OK;
		i += op.size();
		i = readCount(k, i, c.count2);
		if (i >= k.size()) return MORE;
		char32_t last = op.back();
		if (k[i] == last) { c.mot = keysOf("_"); return i + 1 == k.size() ? OK : BAD; }
		if (op.size() == 2 && k[i] == 'g') {
			if (i + 1 >= k.size()) return MORE;
			if (k[i + 1] == last) { c.mot = keysOf("_"); return OK; }
		}
		Parse r = parseMotion(k, i, c, true);
		if (r == NONE) return BAD;
		if (r == OK && i != k.size()) return BAD;
		return r;
	}
	size_t j = i;
	Parse r = parseMotion(k, j, c, visual);
	if (r == OK) return j == k.size() ? OK : BAD;
	if (r == MORE) return MORE;
	if (r == BAD) return BAD;
	c.mot.clear();
	c.arg = 0;
	i++;
	if (a == 'q' && recReg) { c.mot = Keys(1, a); return i == k.size() ? OK : BAD; }
	static const Keys needArg = keysOf("rmq@");
	if (needArg.find(a) != Keys::npos) {
		if (i >= k.size()) return MORE;
		c.arg = k[i++];
		if (c.arg == K_ESC) return BAD;
		c.mot = Keys(1, a);
		return OK;
	}
	if (a == 'g' || a == 'z' || a == 'Z' || a == ctrlKey('w')) {
		if (i >= k.size()) return MORE;
		c.mot = Keys{ a, k[i++] };
		return OK;
	}
	c.mot = Keys(1, a);
	return OK;
}

void Vim::normalKey(char32_t k)
{
	bool visual = md == VISUAL || md == VLINE || md == VBLOCK;
	switch (k) {
	case K_UP: k = 'k'; break;
	case K_DOWN: k = 'j'; break;
	case K_LEFT: k = 'h'; break;
	case K_RIGHT: k = 'l'; break;
	case K_HOME: k = '0'; break;
	case K_END: k = '$'; break;
	case K_PGUP: k = ctrlKey('b'); break;
	case K_PGDN: k = ctrlKey('f'); break;
	case K_DEL: k = visual ? (char32_t)'d' : (char32_t)'x'; break;
	case K_INS: k = visual ? (char32_t)K_ESC : (char32_t)'i'; break;
	}
	if (k == K_ESC && pend.empty()) {
		if (visual) exitVisual();
		return;
	}
	if (k == K_ESC) { pend.clear(); return; }
	pend += k;
	Cmd c;
	Parse r = parse(pend, c, visual);
	if (r == MORE) return;
	if (r == BAD) { pend.clear(); return; }
	Keys keys = pend;
	pend.clear();
	if (c.mot == keysOf("q") && c.op.empty() && !c.arg && recReg) {
		Keys body = recBuf.substr(0, recBuf.size() - std::min(recBuf.size(), keys.size()));
		std::string t;
		for (char32_t x : body) t += ed->encode(x);
		if (isUpper(recReg)) regs[recReg - 'A' + 'a'].text += t;
		else regs[recReg] = Reg{ t, R_CHAR };
		recReg = 0;
		recBuf.clear();
		return;
	}
	if (visual) {
		curChange = Change();
		curChange.reg = c.reg;
		curChange.body = keys.substr(c.body);
		curChange.vmode = md;
		Pos a = vAnchor, z = vCur;
		if (a > z) std::swap(a, z);
		Pos la = ed->lineOf(a), lz = ed->lineOf(z);
		curChange.vLines = lz - la;
		if (md == VBLOCK) {
			Pos ca = ed->column(vAnchor), cz = ed->column(vCur);
			curChange.vCols = want >= INF ? INF : (ca > cz ? ca - cz : cz - ca);
		} else if (la == lz) {
			Pos n = 1;
			for (Pos q = a; q < z; q = ed->after(q)) n++;
			curChange.vCols = n;
		} else curChange.vCols = ed->column(z);
		recordChange = false;
		execVisual(c);
		bool yank = !c.op.empty() ? c.op[0] == 'y' : (c.mot.size() == 1 && (c.mot[0] == 'y' || c.mot[0] == 'Y'));
		if (recordChange && !replaying && !yank) {
			if (insertLike()) lastChange = curChange;
			else { curChange.valid = true; lastChange = curChange; }
		}
	} else {
		curChange = Change();
		curChange.reg = c.reg;
		curChange.count = c.count;
		curChange.body = keys.substr(c.body);
		if (c.count2 && !c.op.empty()) {
			size_t at = c.op.size(), end = at;
			while (end < curChange.body.size() && curChange.body[end] >= '0' && curChange.body[end] <= '9') end++;
			curChange.body.erase(at, end - at);
			curChange.count = (c.count ? c.count : 1) * c.count2;
		}
		recordChange = false;
		exec(c);
		if (recordChange && !replaying) {
			if (insertLike()) lastChange = curChange;
			else { curChange.valid = true; lastChange = curChange; }
		}
	}
}

void Vim::beginChange()
{
	if (!undoOpen) {
		ed->beginUndo();
		undoOpen = true;
		undoDoc = ed->docId();
	}
	recordChange = true;
}

void Vim::endChange()
{
	if (!insertLike() && !holdUndo) closeUndo();
}

void Vim::settleUndo()
{
	if (undoOpen && ed->docId() != undoDoc) {
		staleUndo.push_back(undoDoc);
		undoOpen = false;
	}
	closeUndo();
	auto it = std::find(staleUndo.begin(), staleUndo.end(), ed->docId());
	if (it != staleUndo.end()) {
		staleUndo.erase(it);
		ed->endUndo();
	}
}

void Vim::closeUndo()
{
	if (undoOpen && ed->docId() == undoDoc) {
		ed->endUndo();
		undoOpen = false;
	}
}

Vim::Motion Vim::motion(const Cmd& c, int count, bool forOp)
{
	Motion m;
	Pos p = (md == VISUAL || md == VLINE || md == VBLOCK) ? vCur : ed->caret();
	Pos line = ed->lineOf(p), len = ed->length(), n = ed->lineCount();
	int cnt = count ? count : 1;
	const Keys& k = c.mot;
	char32_t a = k[0];
	m.ok = true;
	m.pos = p;
	auto toLine = [&](Pos l, bool keepCol) {
		l = std::max<Pos>(0, std::min(l, n - 1));
		m.lw = true;
		m.pos = keepCol ? (want >= INF ? ed->lineEnd(l) : ed->posAtColumn(l, want)) : firstNonBlank(l);
	};
	if (k.size() == 2 && a == 'g') {
		char32_t b = k[1];
		if (b == 'g') { toLine(count ? count - 1 : 0, false); m.jump = true; }
		else if (b == 'e' || b == 'E') { for (int i = 0; i < cnt; i++) p = wordEndBack(p, b == 'E'); m.pos = p; m.incl = true; }
		else if (b == '_') {
			Pos l = std::min(line + cnt - 1, n - 1);
			Pos q = ed->lineEnd(l);
			while (q > ed->lineStart(l) && (ed->byteAt(ed->before(q)) == ' ' || ed->byteAt(ed->before(q)) == '\t')) q = ed->before(q);
			m.pos = q > ed->lineStart(l) ? ed->before(q) : q;
			m.incl = true;
		}
		else if (b == 'j') { if (line + cnt >= n) m.ok = false; else toLine(line + cnt, true); }
		else if (b == 'k') { if (line - cnt < 0) m.ok = false; else toLine(line - cnt, true); }
		else if (b == '0') m.pos = ed->lineStart(line);
		else if (b == '$') { m.pos = lineLastChar(line); m.incl = true; }
		else if (b == 'm') m.pos = ed->posAtColumn(line, (ed->column(ed->lineEnd(line))) / 2);
		else m.ok = false;
		return m;
	}
	switch (a) {
	case 'h': case K_BS: {
		Pos ls = ed->lineStart(line);
		if (a == K_BS) { for (int i = 0; i < cnt && p > 0; i++) p = ed->before(p); m.pos = p; break; }
		if (p <= ls) { m.ok = false; break; }
		for (int i = 0; i < cnt && p > ls; i++) p = ed->before(p);
		m.pos = p;
		break;
	}
	case 'l': case ' ': {
		Pos le = ed->lineEnd(line);
		if (a == ' ') { for (int i = 0; i < cnt && p < len; i++) p = ed->after(p); m.pos = p; break; }
		Pos lim = forOp ? le : (le > ed->lineStart(line) ? ed->before(le) : le);
		if (p >= lim) { m.ok = forOp && p < le; m.pos = le; break; }
		for (int i = 0; i < cnt && p < lim; i++) p = ed->after(p);
		m.pos = p;
		break;
	}
	case 'j': if (line + cnt >= n) m.ok = false; else toLine(line + cnt, true); break;
	case 'k': if (line - cnt < 0) m.ok = false; else toLine(line - cnt, true); break;
	case '+': case K_ENTER: if (line + cnt >= n) m.ok = false; else toLine(line + cnt, false); break;
	case '-': if (line - cnt < 0) m.ok = false; else toLine(line - cnt, false); break;
	case '_': toLine(line + cnt - 1, false); break;
	case 'G': toLine(count ? count - 1 : n - 1, false); m.jump = true; break;
	case '0': m.pos = ed->lineStart(line); break;
	case '^': m.pos = firstNonBlank(line); break;
	case '$': {
		Pos l = std::min(line + cnt - 1, n - 1);
		m.pos = forOp ? ed->lineEnd(l) : lineLastChar(l);
		m.incl = !forOp;
		break;
	}
	case '|': m.pos = ed->posAtColumn(line, cnt - 1); if (m.pos > ed->lineEnd(line)) m.pos = ed->lineEnd(line); break;
	case 'w': case 'W': {
		bool big = a == 'W';
		for (int i = 0; i < cnt; i++) {
			bool lastIter = i == cnt - 1;
			Pos q = wordFwd(p, big, forOp && lastIter);
			if (q == p) break;
			p = q;
		}
		if (forOp && p > len) p = len;
		m.pos = p;
		break;
	}
	case 'b': case 'B': for (int i = 0; i < cnt; i++) p = wordBack(p, a == 'B'); m.pos = p; break;
	case 'e': case 'E': for (int i = 0; i < cnt; i++) p = wordEnd(p, a == 'E'); m.pos = p; m.incl = true; break;
	case '{': m.pos = paragraph(p, -1, cnt); m.jump = true; break;
	case '}': m.pos = paragraph(p, 1, cnt); m.jump = true; break;
	case '%': {
		if (count) { toLine((n * count + 99) / 100 - 1, false); m.jump = true; break; }
		Pos q = matchPair(p);
		if (q < 0) { m.ok = false; break; }
		m.pos = q; m.incl = true; m.jump = true;
		break;
	}
	case 'H': case 'M': case 'L': {
		Pos top = ed->topLine(), vis = std::max<Pos>(1, ed->visibleLines());
		Pos bottom = std::min(top + vis - 1, n - 1);
		Pos l = a == 'H' ? top + cnt - 1 : a == 'L' ? bottom - cnt + 1 : (top + bottom) / 2;
		l = std::max(top, std::min(l, bottom));
		toLine(l, false);
		m.jump = true;
		break;
	}
	case 'f': case 'F': case 't': case 'T': {
		Pos q = findChar(p, a, c.arg, cnt, false);
		findCmd = a; findArg = c.arg;
		if (q < 0) { m.ok = false; break; }
		m.pos = q; m.incl = a == 'f' || a == 't';
		break;
	}
	case ';': case ',': {
		if (!findCmd) { m.ok = false; break; }
		char32_t cmd = findCmd;
		if (a == ',') cmd = cmd == 'f' ? 'F' : cmd == 'F' ? 'f' : cmd == 't' ? 'T' : 't';
		Pos q = findChar(p, cmd, findArg, cnt, true);
		if (q < 0) { m.ok = false; break; }
		m.pos = q; m.incl = cmd == 'f' || cmd == 't';
		break;
	}
	case 'n': case 'N': case '*': case '#': {
		bool fwd;
		if (a == '*' || a == '#') {
			Pos ws;
			std::string w = wordUnder(p, ws);
			if (w.empty()) { error("E348: No string under cursor"); m.ok = false; break; }
			lastSearch = "\\<" + utf8(ed->decode(w)) + "\\>";
			lastSearchFwd = a == '*';
			fwd = lastSearchFwd;
			p = ws;
		} else {
			if (lastSearch.empty()) { error("E35: No previous regular expression"); m.ok = false; break; }
			fwd = (a == 'n') == lastSearchFwd;
		}
		Pos s = p, e;
		for (int i = 0; i < cnt; i++) {
			if (!search(lastSearch, fwd, s, s, e, true)) { m.ok = false; break; }
		}
		m.pos = s; m.jump = true;
		break;
	}
	case '\'': case '`': {
		Pos q;
		if (!getMark(c.arg, q)) { error("E20: Mark not set"); m.ok = false; break; }
		if (q > len) q = len;
		if (a == '\'') toLine(ed->lineOf(q), false);
		else m.pos = q;
		m.jump = true;
		break;
	}
	default: m.ok = false;
	}
	return m;
}

Vim::Range Vim::textObject(char32_t ia, char32_t obj, int count)
{
	Range r;
	bool visual = md == VISUAL || md == VLINE || md == VBLOCK;
	Pos p = visual ? vCur : ed->caret();
	Pos line = ed->lineOf(p), ls = ed->lineStart(line), le = ed->lineEnd(line);
	bool inner = ia == 'i';
	if (obj == 'w' || obj == 'W') {
		bool big = obj == 'W';
		if (le == ls) { r.s = ls; r.e = ls; r.ok = !inner; return r; }
		if (p >= le) p = ed->before(le);
		Pos s = p, e = p;
		for (int n = 0; n < std::max(count, 1); n++) {
			int c = cls(e, big);
			if (n == 0) { while (s > ls && cls(ed->before(s), big) == c) s = ed->before(s); }
			while (e < le && cls(e, big) == c) e = ed->after(e);
			if (!inner) {
				if (c != 0) {
					Pos e2 = e;
					while (e2 < le && cls(e2, big) == 0) e2 = ed->after(e2);
					if (e2 > e) e = e2;
					else if (n == 0) while (s > ls && cls(ed->before(s), big) == 0) s = ed->before(s);
				} else {
					int c2 = cls(e, big);
					while (e < le && cls(e, big) == c2 && c2 != 0) e = ed->after(e);
				}
			}
		}
		r.s = s; r.e = e; r.ok = true;
		return r;
	}
	if (obj == 'p') {
		Pos n = ed->lineCount();
		bool blank = isBlankLine(line);
		Pos l1 = line, l2 = line;
		while (l1 > 0 && isBlankLine(l1 - 1) == blank) l1--;
		while (l2 + 1 < n && isBlankLine(l2 + 1) == blank) l2++;
		if (!inner) {
			if (blank) { while (l2 + 1 < n && !isBlankLine(l2 + 1)) l2++; }
			else if (l2 + 1 < n) { while (l2 + 1 < n && isBlankLine(l2 + 1)) l2++; }
			else { while (l1 > 0 && isBlankLine(l1 - 1)) l1--; }
		}
		r.lw = true; r.l1 = l1; r.l2 = l2; r.ok = true;
		r.s = ed->lineStart(l1); r.e = ed->lineEnd(l2);
		return r;
	}
	if (obj == '"' || obj == '\'' || obj == '`') {
		int q = (int)obj;
		std::vector<Pos> qs;
		for (Pos i = ls; i < le; i++) {
			int b = ed->byteAt(i);
			if (b == '\\') { i++; continue; }
			if (b == q) qs.push_back(i);
		}
		Pos s = -1, e = -1;
		for (size_t i = 0; i + 1 < qs.size(); i += 2) {
			if (qs[i] <= p && p <= qs[i + 1]) { s = qs[i]; e = qs[i + 1]; break; }
		}
		if (s < 0) for (size_t i = 0; i + 1 < qs.size(); i += 2) if (qs[i] > p) { s = qs[i]; e = qs[i + 1]; break; }
		if (s < 0) return r;
		if (inner) { r.s = s + 1; r.e = e; }
		else {
			r.s = s; r.e = e + 1;
			Pos t = r.e;
			while (t < le && (ed->byteAt(t) == ' ' || ed->byteAt(t) == '\t')) t++;
			if (t > r.e) r.e = t;
			else while (r.s > ls && (ed->byteAt(r.s - 1) == ' ' || ed->byteAt(r.s - 1) == '\t')) r.s--;
		}
		r.ok = true;
		return r;
	}
	int open, close;
	switch (obj) {
	case '(': case ')': case 'b': open = '('; close = ')'; break;
	case '{': case '}': case 'B': open = '{'; close = '}'; break;
	case '[': case ']': open = '['; close = ']'; break;
	default: open = '<'; close = '>'; break;
	}
	Pos s = -1;
	int nest = 0;
	Pos q = p < ed->length() && ed->byteAt(p) == close ? p - 1 : p;
	for (int n = 0; n < std::max(count, 1); n++) {
		nest = 0;
		s = -1;
		for (; q >= 0; q--) {
			int b = ed->byteAt(q);
			if (b == close) nest++;
			else if (b == open) { if (nest == 0) { s = q; break; } nest--; }
		}
		if (s < 0) return r;
		q = s - 1;
	}
	Pos len = ed->length(), e = -1;
	nest = 0;
	for (Pos t = s + 1; t < len; t++) {
		int b = ed->byteAt(t);
		if (b == open) nest++;
		else if (b == close) { if (nest == 0) { e = t; break; } nest--; }
	}
	if (e < 0) return r;
	r.ok = true;
	if (!inner) { r.s = s; r.e = e + 1; return r; }
	r.s = s + 1; r.e = e;
	Pos sl = ed->lineOf(s), el = ed->lineOf(e);
	if (sl != el && ed->lineEnd(sl) == r.s && firstNonBlank(el) == e) {
		if (el - sl >= 2) { r.lw = true; r.l1 = sl + 1; r.l2 = el - 1; r.s = ed->lineStart(r.l1); r.e = ed->lineEnd(r.l2); }
		else { r.s = ed->lineStart(el); r.e = ed->lineStart(el); }
	}
	return r;
}

Vim::Range Vim::opRange(const Cmd& c, int count)
{
	Range r;
	Pos p = ed->caret();
	if (c.mot.size() == 2 && (c.mot[0] == 'i' || c.mot[0] == 'a')) {
		r = textObject(c.mot[0], c.mot[1], count);
		if (r.ok && r.lw) { r.l1 = ed->lineOf(r.s); r.l2 = ed->lineOf(r.e); }
		return r;
	}
	Cmd mc = c;
	if (c.op == keysOf("c") && (c.mot == keysOf("w") || c.mot == keysOf("W")) && cls(p, c.mot[0] == 'W') != 0) {
		mc.mot = Keys(1, c.mot[0] == 'w' ? 'e' : 'E');
		Pos q = p;
		int cnt = count ? count : 1;
		for (int i = 0; i < cnt; i++) {
			bool big = mc.mot[0] == 'E';
			if (i == 0 && ed->after(q) < ed->length() && cls(ed->after(q), big) == cls(q, big)) q = wordEnd(q, big);
			else if (i > 0) q = wordEnd(q, big);
		}
		r.s = p; r.e = ed->after(q); r.ok = true;
		return r;
	}
	Motion m = motion(mc, count, true);
	if (!m.ok) return r;
	r.ok = true;
	if (m.jump) setMark('\'', p);
	if (m.lw) {
		r.lw = true;
		r.l1 = std::min(ed->lineOf(p), ed->lineOf(m.pos));
		r.l2 = std::max(ed->lineOf(p), ed->lineOf(m.pos));
		r.s = ed->lineStart(r.l1);
		r.e = ed->lineEnd(r.l2);
		return r;
	}
	r.s = std::min(p, m.pos);
	r.e = std::max(p, m.pos);
	if (m.incl) r.e = ed->after(r.e);
	if (!m.incl && ed->lineOf(r.s) != ed->lineOf(r.e) && r.e == ed->lineStart(ed->lineOf(r.e)) && c.mot[0] != '`') {
		Pos l1 = ed->lineOf(r.s), l2 = ed->lineOf(r.e) - 1;
		if (r.s <= firstNonBlank(l1)) { r.lw = true; r.l1 = l1; r.l2 = l2; r.s = ed->lineStart(l1); r.e = ed->lineEnd(l2); }
		else r.e = ed->lineEnd(l2);
	}
	return r;
}

Vim::Range Vim::visualRange()
{
	Range r;
	r.ok = true;
	Pos a = vAnchor, c = vCur;
	if (md == VLINE) {
		r.lw = true;
		r.l1 = std::min(ed->lineOf(a), ed->lineOf(c));
		r.l2 = std::max(ed->lineOf(a), ed->lineOf(c));
		r.s = ed->lineStart(r.l1);
		r.e = ed->lineEnd(r.l2);
	} else if (md == VBLOCK) {
		r.block = true;
		r.l1 = std::min(ed->lineOf(a), ed->lineOf(c));
		r.l2 = std::max(ed->lineOf(a), ed->lineOf(c));
		Pos ca = ed->column(a), cc = ed->column(c);
		r.c1 = std::min(ca, cc);
		Pos hi = std::max(ca, cc);
		Pos hp = ca > cc ? a : c;
		r.c2 = want >= INF ? INF : (hp < ed->length() ? ed->column(ed->after(hp)) : hi + 1);
		if (r.c2 <= hi) r.c2 = hi + 1;
		r.s = ed->posAtColumn(r.l1, r.c1);
		r.e = ed->posAtColumn(r.l2, r.c2);
	} else {
		r.s = std::min(a, c);
		r.e = std::min(ed->after(std::max(a, c)), ed->length());
		Pos hl = ed->lineOf(std::max(a, c));
		if (want >= INF && c >= a && hl + 1 < ed->lineCount()) r.e = ed->lineStart(hl + 1);
	}
	return r;
}

void Vim::setReg(char32_t r, const std::string& text, int type, bool del)
{
	if (r == '_') return;
	Reg v{ text, type };
	if (r == '+' || r == '*') {
		ed->setClipboard(text);
		lastClip = text;
		lastClipType = type;
		regs['"'] = v;
		return;
	}
	if (isUpper(r)) {
		char32_t lr = r - 'A' + 'a';
		Reg& old = regs[lr];
		if (old.type == R_LINE || type == R_LINE) {
			std::string t = old.text;
			if (old.type != R_LINE && !t.empty()) t += ed->eol();
			t += text;
			if (type != R_LINE) t += ed->eol();
			old.text = t;
			old.type = R_LINE;
		} else old.text += text;
		regs['"'] = old;
		return;
	}
	if (r == 0 || r == '"') {
		if (del) {
			if (type == R_LINE || text.find('\n') != std::string::npos) {
				for (char32_t i = '9'; i > '1'; i--) regs[i] = regs[i - 1];
				regs['1'] = v;
			} else regs['-'] = v;
		} else regs['0'] = v;
		if (clipboardUnnamed) { ed->setClipboard(text); lastClip = text; lastClipType = type; }
	} else regs[r] = v;
	regs['"'] = v;
}

Reg Vim::getReg(char32_t r)
{
	if (r == 0) r = '"';
	if (isUpper(r)) r = r - 'A' + 'a';
	if (r == '+' || r == '*' || (r == '"' && clipboardUnnamed)) {
		std::string t = ed->clipboard();
		if (t.empty() && r == '"') return regs['"'];
		Reg v{ t, R_CHAR };
		if (t == lastClip) v.type = lastClipType;
		else if (!t.empty() && t.back() == '\n') v.type = R_LINE;
		return v;
	}
	if (r == '/') return Reg{ lastSearch, R_CHAR };
	if (r == '.') return Reg{ lastInserted, R_CHAR };
	auto it = regs.find(r);
	return it == regs.end() ? Reg() : it->second;
}

void Vim::shiftLines(Pos l1, Pos l2, int dir, int times)
{
	int w = ed->indentWidth();
	for (Pos l = l1; l <= l2; l++) {
		if (dir > 0 && isBlankLine(l)) continue;
		Pos cur = ed->lineIndent(l);
		Pos nw = dir > 0 ? (cur / w + times) * w : std::max<Pos>(0, ((cur + w - 1) / w - times) * w);
		ed->setLineIndent(l, nw);
	}
}

void Vim::applyOp(const Keys& op, Range r, char32_t reg, int count)
{
	char32_t o = op[0];
	std::string eol = ed->eol();
	Pos len = ed->length(), n = ed->lineCount();
	if (o == 'y' || o == 'd' || o == 'c') {
		std::string text;
		int type = R_CHAR;
		if (r.block) {
			type = R_BLOCK;
			for (Pos l = r.l1; l <= r.l2; l++) {
				Pos a = ed->posAtColumn(l, r.c1), b = r.c2 >= INF ? ed->lineEnd(l) : ed->posAtColumn(l, r.c2);
				text += ed->text(a, b);
				if (l < r.l2) text += eol;
			}
		} else if (r.lw) {
			type = R_LINE;
			Pos e = r.l2 + 1 < n ? ed->lineStart(r.l2 + 1) : len;
			text = ed->text(ed->lineStart(r.l1), e);
			if (r.l2 + 1 >= n) text += eol;
		} else text = ed->text(r.s, r.e);
		setReg(reg, text, type, o != 'y');
		if (o == 'y') {
			Pos to = r.block ? ed->posAtColumn(r.l1, r.c1) : r.lw ? (ed->lineOf(ed->caret()) == r.l1 && md == NORMAL ? ed->caret() : ed->posAtColumn(r.l1, ed->column(ed->caret()))) : r.s;
			if (md != NORMAL) setMode(NORMAL);
			moveTo(to);
			return;
		}
		beginChange();
		if (r.block) {
			for (Pos l = r.l2; l >= r.l1; l--) {
				Pos a = ed->posAtColumn(l, r.c1), b = r.c2 >= INF ? ed->lineEnd(l) : ed->posAtColumn(l, r.c2);
				if (b > a) ed->remove(a, b);
			}
			Pos p = ed->posAtColumn(r.l1, r.c1);
			if (o == 'c') {
				setMode(NORMAL);
				moveTo(p);
				ed->select(p, p);
				blockIns = r.l2 > r.l1;
				blockL1 = r.l1; blockL2 = r.l2; blockCol = ed->column(p); blockPad = false; blockEnd = false;
				startInsert(1, Keys());
				return;
			}
			setMode(NORMAL);
			moveTo(p);
			endChange();
			return;
		}
		if (r.lw) {
			if (o == 'c') {
				std::string ind = indentOf(r.l1);
				ed->remove(ed->lineStart(r.l1), ed->lineEnd(r.l2));
				ed->insert(ed->lineStart(r.l1), ind);
				Pos p = ed->lineStart(r.l1) + (Pos)ind.size();
				setMode(NORMAL);
				ed->select(p, p);
				startInsert(1, Keys());
				return;
			}
			if (r.l2 + 1 < n) ed->remove(ed->lineStart(r.l1), ed->lineStart(r.l2 + 1));
			else if (r.l1 > 0) ed->remove(ed->lineEnd(r.l1 - 1), len);
			else ed->remove(0, len);
			setMode(NORMAL);
			moveTo(firstNonBlank(std::min(r.l1, ed->lineCount() - 1)));
			endChange();
			return;
		}
		ed->remove(r.s, r.e);
		setMode(NORMAL);
		if (o == 'c') { ed->select(r.s, r.s); startInsert(1, Keys()); return; }
		moveTo(r.s);
		endChange();
		return;
	}
	beginChange();
	Pos l1 = r.block || r.lw ? r.l1 : ed->lineOf(r.s);
	Pos l2 = r.block || r.lw ? r.l2 : ed->lineOf(r.e > r.s ? ed->before(r.e) : r.e);
	if (o == '>' || o == '<') {
		shiftLines(l1, l2, o == '>' ? 1 : -1, std::max(count, 1));
		setMode(NORMAL);
		moveTo(firstNonBlank(l1));
	} else if (o == '=') {
		setMode(NORMAL);
		moveTo(firstNonBlank(l1));
	} else {
		CaseMode cm = op == keysOf("gU") || op == keysOf("U") ? CASE_UPPER : op == keysOf("gu") || op == keysOf("u") ? CASE_LOWER : CASE_TOGGLE;
		if (r.block) {
			for (Pos l = r.l1; l <= r.l2; l++) {
				Pos a = ed->posAtColumn(l, r.c1), b = r.c2 >= INF ? ed->lineEnd(l) : ed->posAtColumn(l, r.c2);
				if (b > a) { std::string t = ed->mapCase(ed->text(a, b), cm); ed->remove(a, b); ed->insert(a, t); }
			}
			r.s = ed->posAtColumn(r.l1, r.c1);
		} else {
			if (r.lw) { r.s = ed->lineStart(r.l1); r.e = ed->lineEnd(r.l2); }
			std::string t = ed->mapCase(ed->text(r.s, r.e), cm);
			ed->remove(r.s, r.e);
			ed->insert(r.s, t);
		}
		setMode(NORMAL);
		moveTo(r.s);
	}
	endChange();
}

void Vim::startInsert(int count, const Keys& cmd)
{
	ed->setOvertype(false);
	insCount = std::max(count, 1);
	insCmd = cmd;
	insKeys.clear();
	insStart = ed->caret();
	recordChange = true;
	if (!undoOpen) beginChange();
	setMode(INSERT);
	want = ed->column(ed->caret());
}

void Vim::exitInsert()
{
	ed->cancelPopups();
	bool repl = md == REPLACE;
	if (repl) ed->setOvertype(false);
	Keys typed = insKeys;
	if (insCount > 1) {
		bool was = replaying;
		replaying = true;
		Mode keep = md;
		for (int i = 1; i < insCount; i++) {
			if (insCmd == keysOf("o") || insCmd == keysOf("O")) {
				Pos line = curLine();
				std::string ind = indentOf(line);
				if (insCmd[0] == 'o') { ed->insert(ed->lineEnd(line), ed->eol() + ind); Pos p = ed->lineStart(line + 1) + (Pos)ind.size(); ed->select(p, p); }
				else { Pos ls = ed->lineStart(line); ed->insert(ls, ind + ed->eol()); Pos p = ls + (Pos)ind.size(); ed->select(p, p); }
			}
			for (char32_t k : typed) insertKey(k);
		}
		md = keep;
		replaying = was;
		insCount = 1;
	}
	bool wasBlock = blockIns;
	if (blockIns) {
		blockIns = false;
		Pos line = ed->lineOf(insStart);
		Pos c = ed->caret();
		if (ed->lineOf(c) == line && c > insStart) {
			std::string t = ed->text(insStart, c);
			for (Pos l = blockL1 + 1; l <= blockL2; l++) {
				Pos endCol = ed->column(ed->lineEnd(l));
				Pos col = blockEnd ? endCol : blockCol;
				if (endCol < col) {
					if (!blockPad) continue;
					ed->insert(ed->lineEnd(l), std::string((size_t)(col - endCol), ' '));
				}
				ed->insert(ed->posAtColumn(l, col), t);
			}
			ed->select(insStart, insStart);
		}
	}
	{
		std::vector<std::string> parts;
		for (char32_t k : typed) {
			if (k == K_BS) { if (!parts.empty()) parts.pop_back(); }
			else if (k == K_ENTER) parts.push_back(ed->eol());
			else if (k == K_TAB) parts.push_back("\t");
			else if (k >= 32 && k < K_UP) parts.push_back(ed->encode(k));
		}
		lastInserted.clear();
		for (auto& p : parts) lastInserted += p;
	}
	if (recordChange) {
		curChange.ins = typed;
		curChange.ins += K_ESC;
		curChange.valid = true;
		if (!replaying) lastChange = curChange;
	}
	setMark('^', ed->caret());
	setMark('.', ed->caret());
	setMode(NORMAL);
	Pos p = ed->caret();
	if (wasBlock) p = insStart;
	else if (p > ed->lineStart(ed->lineOf(p))) p = ed->before(p);
	moveTo(p);
	closeUndo();
}

void Vim::insertKey(char32_t k)
{
	if (insPending) {
		insPending = false;
		if (k == K_ESC) return;
		Reg r = getReg(k);
		if (!r.text.empty()) {
			Pos p = ed->caret();
			ed->insert(p, r.text);
			p += (Pos)r.text.size();
			ed->select(p, p);
		}
		return;
	}
	if (k == K_ESC) { exitInsert(); return; }
	if (k == ctrlKey('r')) { insPending = true; return; }
	insKeys += k;
	Pos p = ed->caret();
	if (ed->anchor() != p) {
		Pos a = std::min(ed->anchor(), p), b = std::max(ed->anchor(), p);
		ed->remove(a, b);
		p = a;
	}
	Pos line = ed->lineOf(p), ls = ed->lineStart(line);
	if (k == ctrlKey('w')) {
		if (p <= ls) { if (p > 0) { Pos q = ed->before(p); ed->remove(q, p); p = q; } }
		else {
			Pos q = p;
			while (q > ls && (ed->byteAt(q - 1) == ' ' || ed->byteAt(q - 1) == '\t')) q--;
			int c = q > ls ? cls(ed->before(q), false) : 0;
			while (q > ls && cls(ed->before(q), false) == c && c != 0) q = ed->before(q);
			ed->remove(q, p);
			p = q;
		}
	} else if (k == ctrlKey('u')) {
		Pos q = p > firstNonBlank(line) ? firstNonBlank(line) : ls;
		ed->remove(q, p);
		p = q;
	} else if (k == K_BS) {
		if (p > 0) { Pos q = ed->before(p); ed->remove(q, p); p = q; }
	} else if (k == K_ENTER) {
		std::string ind = indentOf(line);
		if (p < ls + (Pos)ind.size()) ind = ind.substr(0, (size_t)(p - ls));
		std::string s = ed->eol() + ind;
		ed->insert(p, s);
		p += (Pos)s.size();
	} else if (k == K_TAB) {
		std::string s = "\t";
		if (!ed->useTabs()) { int w = ed->tabWidth(); s = std::string((size_t)(w - ed->column(p) % w), ' '); }
		ed->insert(p, s);
		p += (Pos)s.size();
	} else if (k >= 32 && (k < 0xE000 || k > 0xF8FF)) {
		std::string s = ed->encode(k);
		if (md == REPLACE && p < ed->lineEnd(line)) ed->remove(p, ed->after(p));
		ed->insert(p, s);
		p += (Pos)s.size();
	}
	ed->select(p, p);
}

void Vim::enterVisual(Mode m)
{
	if (md == NORMAL) { vAnchor = ed->caret(); vCur = vAnchor; }
	setMode(m);
	renderVisual();
}

void Vim::exitVisual()
{
	lastVisual = md;
	lastVA = vAnchor;
	lastVC = vCur;
	setMark('<', std::min(vAnchor, vCur));
	setMark('>', std::max(vAnchor, vCur));
	setMode(NORMAL);
	moveTo(vCur);
}

void Vim::renderVisual()
{
	Pos a = vAnchor, c = vCur, len = ed->length();
	if (md == VISUAL) {
		if (c >= a) ed->select(a, std::min(ed->after(c), len));
		else ed->select(std::min(ed->after(a), len), c);
	} else if (md == VLINE) {
		Pos la = ed->lineOf(a), lc = ed->lineOf(c);
		if (lc >= la) ed->select(ed->lineStart(la), ed->lineEnd(lc));
		else ed->select(ed->lineEnd(la), ed->lineStart(lc));
	} else if (md == VBLOCK) {
		Pos ca = ed->column(a), cc = ed->column(c);
		Pos ea = ca, ec = cc;
		if (cc >= ca) ec = c < len ? ed->column(ed->after(c)) : cc + 1;
		else ea = a < len ? ed->column(ed->after(a)) : ca + 1;
		if (want >= INF) {
			Pos mx = 0;
			for (Pos l = std::min(ed->lineOf(a), ed->lineOf(c)); l <= std::max(ed->lineOf(a), ed->lineOf(c)); l++) mx = std::max(mx, ed->column(ed->lineEnd(l)));
			if (cc >= ca) ec = std::max(mx, ec); else ea = std::max(mx, ea);
		}
		ed->selectBlock(ed->lineOf(a), ea, ed->lineOf(c), ec);
	}
	syncSel();
}

void Vim::put(char32_t reg, int count, bool before, bool keepUnnamed)
{
	(void)keepUnnamed;
	Reg r = getReg(reg);
	{
		std::string eolS = ed->eol(), t;
		t.reserve(r.text.size());
		for (size_t i = 0; i < r.text.size(); i++) {
			char ch = r.text[i];
			if (ch == '\r') { if (i + 1 < r.text.size() && r.text[i + 1] == '\n') i++; t += eolS; }
			else if (ch == '\n') t += eolS;
			else t += ch;
		}
		r.text = t;
	}
	if (r.text.empty()) { error("E353: Nothing in register " + utf8(Keys(1, reg ? reg : '"'))); return; }
	int cnt = std::max(count, 1);
	beginChange();
	Pos p = ed->caret(), line = ed->lineOf(p);
	std::string eol = ed->eol();
	if (r.type == R_LINE) {
		std::string t;
		for (int i = 0; i < cnt; i++) t += r.text;
		Pos n = ed->lineCount();
		if (before) ed->insert(ed->lineStart(line), t);
		else if (line + 1 < n) ed->insert(ed->lineStart(line + 1), t);
		else {
			while (!t.empty() && (t.back() == 10 || t.back() == 13)) t.pop_back();
			ed->insert(ed->length(), eol + t);
		}
		Pos target = before ? line : line + 1;
		moveTo(firstNonBlank(target));
	} else if (r.type == R_BLOCK) {
		std::vector<std::string> parts;
		size_t s = 0;
		for (;;) {
			size_t e = r.text.find(eol, s);
			parts.push_back(r.text.substr(s, e == std::string::npos ? std::string::npos : e - s));
			if (e == std::string::npos) break;
			s = e + eol.size();
		}
		Pos col = ed->column(p);
		if (!before && p < ed->lineEnd(line)) col = ed->column(ed->after(p));
		for (size_t i = 0; i < parts.size(); i++) {
			Pos l = line + (Pos)i;
			if (l >= ed->lineCount()) ed->insert(ed->length(), eol);
			Pos endCol = ed->column(ed->lineEnd(l));
			if (endCol < col) ed->insert(ed->lineEnd(l), std::string((size_t)(col - endCol), ' '));
			std::string t;
			for (int c = 0; c < cnt; c++) t += parts[i];
			ed->insert(ed->posAtColumn(l, col), t);
		}
		moveTo(ed->posAtColumn(line, col));
	} else {
		std::string t;
		for (int i = 0; i < cnt; i++) t += r.text;
		Pos at = p;
		if (!before && p < ed->lineEnd(line)) at = ed->after(p);
		ed->insert(at, t);
		Pos end = at + (Pos)t.size();
		if (t.find('\n') != std::string::npos || t.find('\r') != std::string::npos) moveTo(at);
		else moveTo(end > at ? ed->before(end) : at);
	}
	endChange();
}

void Vim::joinLines(Pos line, int count, bool spaces)
{
	int joins = std::max(count, 2) - 1;
	if (line + 1 >= ed->lineCount()) return;
	beginChange();
	Pos at = 0;
	for (int i = 0; i < joins && line + 1 < ed->lineCount(); i++) {
		Pos e = ed->lineEnd(line);
		Pos ns = ed->lineStart(line + 1);
		Pos nb = spaces ? firstNonBlank(line + 1) : ns;
		bool nextEmpty = nb == ed->lineEnd(line + 1);
		Pos te = e;
		bool trailing = te > ed->lineStart(line) && (ed->byteAt(te - 1) == ' ' || ed->byteAt(te - 1) == '\t');
		ed->remove(te, nb);
		at = te;
		if (spaces && !nextEmpty && !trailing && te > ed->lineStart(line) && ed->byteAt(te) != ')') ed->insert(te, " ");
	}
	moveTo(at);
	endChange();
}

void Vim::replaceChars(char32_t c, int count)
{
	int cnt = std::max(count, 1);
	Pos p = ed->caret(), le = ed->lineEnd(ed->lineOf(p));
	Pos q = p;
	for (int i = 0; i < cnt; i++) { if (q >= le) { return; } q = ed->after(q); }
	beginChange();
	ed->remove(p, q);
	if (c == K_ENTER) {
		ed->insert(p, ed->eol());
		moveTo(ed->lineStart(ed->lineOf(p) + 1));
	} else {
		std::string s, one = ed->encode(c);
		for (int i = 0; i < cnt; i++) s += one;
		ed->insert(p, s);
		moveTo(p + (Pos)s.size() - (Pos)one.size());
	}
	endChange();
}

void Vim::addNumber(int delta)
{
	Pos p = ed->caret(), line = ed->lineOf(p), ls = ed->lineStart(line), le = ed->lineEnd(line);
	auto dig = [&](Pos q) { int b = ed->byteAt(q); return b >= '0' && b <= '9'; };
	Pos s = p;
	if (s < le && dig(s)) { while (s > ls && dig(s - 1)) s--; }
	else { while (s < le && !dig(s)) s++; if (s >= le) return; }
	Pos e = s;
	while (e < le && dig(e)) e++;
	bool neg = s > ls && ed->byteAt(s - 1) == '-';
	if (neg) s--;
	long long v = std::strtoll(ed->text(s, e).c_str(), nullptr, 10) + delta;
	std::string t = std::to_string(v);
	beginChange();
	ed->remove(s, e);
	ed->insert(s, t);
	moveTo(s + (Pos)t.size() - 1);
	endChange();
}

void Vim::scroll(char32_t k, int count)
{
	Pos top = ed->topLine(), vis = std::max<Pos>(1, ed->visibleLines()), n = ed->lineCount();
	Pos line = curLine();
	int cnt = std::max(count, 1);
	auto go = [&](Pos l) {
		l = std::max<Pos>(0, std::min(l, n - 1));
		moveTo(want >= INF ? ed->lineEnd(l) : ed->posAtColumn(l, want), true);
	};
	if (k == ctrlKey('e')) {
		ed->setTopLine(top + cnt);
		Pos nt = ed->topLine();
		if (line < nt) go(nt);
	} else if (k == ctrlKey('y')) {
		ed->setTopLine(std::max<Pos>(0, top - cnt));
		Pos nt = ed->topLine();
		if (line > nt + vis - 1) go(nt + vis - 1);
	} else if (k == ctrlKey('d') || k == ctrlKey('u')) {
		Pos half = count ? count : std::max<Pos>(1, vis / 2);
		Pos d = k == ctrlKey('d') ? half : -half;
		if ((d > 0 && line >= n - 1) || (d < 0 && line <= 0)) return;
		ed->setTopLine(std::max<Pos>(0, top + d));
		go(line + d);
		Pos l = curLine();
		moveTo(firstNonBlank(l));
	} else if (k == ctrlKey('f') || k == ctrlKey('b')) {
		Pos page = std::max<Pos>(1, vis - 2) * cnt;
		if (k == ctrlKey('f')) { Pos nt = std::min(top + page, std::max<Pos>(0, n - 1)); ed->setTopLine(nt); go(ed->topLine()); }
		else { Pos nt = std::max<Pos>(0, top - page); ed->setTopLine(nt); go(std::min(nt + vis - 1, n - 1)); }
		moveTo(firstNonBlank(curLine()));
	}
}

void Vim::setMark(char32_t m, Pos p) { marks[ed->docId()][m] = p; }

bool Vim::getMark(char32_t m, Pos& p)
{
	if (m == '`') m = '\'';
	auto& mm = marks[ed->docId()];
	auto it = mm.find(m);
	if (it == mm.end()) return false;
	p = it->second;
	return true;
}

std::string Vim::toDoc(const std::string& u)
{
	bool ascii = true;
	for (char c : u) if ((unsigned char)c >= 0x80) { ascii = false; break; }
	if (ascii) return u;
	std::string out;
	for (char32_t c : fromUtf8(u)) out += ed->encode(c);
	return out;
}

std::string Vim::toRegex(const std::string& v, bool& icase)
{
	std::string out;
	bool very = false;
	for (size_t i = 0; i < v.size(); i++) {
		char c = v[i];
		if (c == '\\' && i + 1 < v.size()) {
			char d = v[++i];
			if (d == 'v') { very = true; continue; }
			if (d == 'V' || d == 'm' || d == 'M') { very = false; continue; }
			if (d == 'c') { icase = true; continue; }
			if (d == 'C') { icase = false; continue; }
			if (d == '<' || d == '>') { out += "\\b"; continue; }
			if (!very && (d == '(' || d == ')' || d == '|' || d == '+' || d == '?' || d == '{' || d == '}')) { out += d; continue; }
			if (!very && d == '=') { out += '?'; continue; }
			out += '\\';
			out += d;
			continue;
		}
		if (very) {
			if (c == '<' || c == '>') { out += "\\b"; continue; }
			if (c == '=') { out += '?'; continue; }
			out += c;
			continue;
		}
		if (c == '(' || c == ')' || c == '|' || c == '+' || c == '?' || c == '{' || c == '}') { out += '\\'; out += c; continue; }
		out += c;
	}
	return out;
}

bool Vim::search(const std::string& vpat, bool fwd, Pos from, Pos& s, Pos& e, bool wrapMsg)
{
	bool icase = false;
	std::string re = toDoc(toRegex(vpat, icase));
	Pos len = ed->length();
	if (fwd) {
		Pos start = from < len ? ed->after(from) : len;
		if (ed->find(re, start, len, icase, s, e)) return true;
		if (ed->find(re, 0, std::min(start, len), icase, s, e)) { if (wrapMsg) error("search hit BOTTOM, continuing at TOP"); return true; }
	} else {
		if (from > 0 && findBack(re, from, 0, icase, s, e)) return true;
		if (findBack(re, len + 1, from, icase, s, e)) { if (wrapMsg) error("search hit TOP, continuing at BOTTOM"); return true; }
	}
	error("E486: Pattern not found: " + vpat);
	return false;
}

bool Vim::findBack(const std::string& re, Pos from, Pos to, bool icase, Pos& s, Pos& e)
{
	Pos len = ed->length();
	Pos top = ed->lineOf(std::min(to, len));
	for (Pos l = ed->lineOf(std::min(from, len)); l >= top; l--) {
		Pos q = std::max(ed->lineStart(l), to), le = ed->lineEnd(l);
		Pos bs = -1, be = -1, ms, me;
		while (q <= le && q < from && ed->find(re, q, le, icase, ms, me) && ms < from) {
			bs = ms; be = me;
			q = me > ms ? me : ed->after(ms);
			if (ms >= le) break;
		}
		if (bs >= 0) { s = bs; e = be; return true; }
	}
	return false;
}

Vim::Range Vim::lineRange(const Range& r)
{
	Range o = r;
	if (!r.block && !r.lw) { o.l1 = ed->lineOf(r.s); o.l2 = ed->lineOf(r.e > r.s ? ed->before(r.e) : r.e); }
	o.block = false;
	o.lw = true;
	o.s = ed->lineStart(o.l1);
	o.e = ed->lineEnd(o.l2);
	return o;
}

void Vim::runKeys(const Keys& k)
{
	if (runDepth > 20) return;
	runDepth++;
	for (char32_t c : k) feed(c);
	runDepth--;
}

void Vim::dot(int count)
{
	if (!lastChange.valid) return;
	Change ch = lastChange;
	Keys k;
	if (ch.reg) { k += '"'; k += ch.reg; }
	int n = count ? count : ch.count;
	if (n && ch.vmode == NORMAL) for (char c : std::to_string(n)) k += (char32_t)c;
	k += ch.body;
	bool was = replaying;
	replaying = true;
	if (ch.vmode != NORMAL) {
		Pos p = ed->caret(), l = ed->lineOf(p);
		Pos lz = std::min(l + ch.vLines, ed->lineCount() - 1);
		Pos z = p;
		if (ch.vmode == VLINE) z = ed->lineStart(lz);
		else if (ch.vmode == VBLOCK) z = ch.vCols >= INF ? ed->lineEnd(lz) : ed->posAtColumn(lz, ed->column(p) + ch.vCols);
		else if (ch.vLines == 0) { for (Pos i = 1; i < ch.vCols && z < ed->lineEnd(l); i++) z = ed->after(z); }
		else z = ed->posAtColumn(lz, ch.vCols);
		if (ch.vmode != VLINE) z = clampNormal(z);
		setMode(ch.vmode);
		vAnchor = p;
		vCur = z;
		want = ch.vmode == VBLOCK && ch.vCols >= INF ? INF : ed->column(z);
		renderVisual();
	}
	runKeys(k);
	if (insertLike()) {
		Keys ins = ch.ins;
		if (!ins.empty() && ins.back() == K_ESC) ins.pop_back();
		for (char32_t c : ins) insertKey(c);
		insKeys = ins;
		exitInsert();
	}
	replaying = was;
	if (count) lastChange.count = count;
}

bool Vim::simple(Cmd& c)
{
	const Keys& k = c.mot;
	char32_t a = k[0];
	int cnt = std::max(c.count, 1);
	Pos p = ed->caret(), line = ed->lineOf(p);
	auto asOp = [&](const char* op, const char* mot) {
		Cmd d = c;
		d.op = keysOf(op);
		d.mot = keysOf(mot);
		exec(d);
	};
	if (k.size() == 1) {
		switch (a) {
		case 'x': if (ed->lineStart(line) == ed->lineEnd(line)) return true; asOp("d", "l"); return true;
		case 'X': asOp("d", "h"); return true;
		case 'D': asOp("d", "$"); return true;
		case 'C': asOp("c", "$"); return true;
		case 's': if (ed->lineStart(line) == ed->lineEnd(line)) { beginChange(); startInsert(c.count, Keys()); return true; } asOp("c", "l"); return true;
		case 'S': asOp("c", "_"); return true;
		case 'Y': asOp("y", "_"); return true;
		case 'p': case 'P': put(c.reg, c.count, a == 'P'); return true;
		case 'J': joinLines(line, c.count, true); return true;
		case 'u': for (int i = 0; i < cnt; i++) ed->undo(); moveTo(clampNormal(ed->caret())); return true;
		case 0x12: for (int i = 0; i < cnt; i++) ed->redo(); moveTo(clampNormal(ed->caret())); return true;
		case '.': dot(c.count); return true;
		case '~': {
			Pos le = ed->lineEnd(line);
			if (p >= le) return true;
			Pos q = p;
			for (int i = 0; i < cnt && q < le; i++) q = ed->after(q);
			beginChange();
			std::string t = ed->mapCase(ed->text(p, q), CASE_TOGGLE);
			ed->remove(p, q);
			ed->insert(p, t);
			moveTo(q);
			endChange();
			return true;
		}
		case 'r': replaceChars(c.arg, c.count); return true;
		case 'i': beginChange(); startInsert(c.count, Keys(1, a)); return true;
		case 'a': {
			Pos q = p < ed->lineEnd(line) ? ed->after(p) : p;
			ed->select(q, q);
			beginChange();
			startInsert(c.count, Keys(1, a));
			return true;
		}
		case 'I': { Pos q = firstNonBlank(line); ed->select(q, q); beginChange(); startInsert(c.count, Keys(1, a)); return true; }
		case 'A': { Pos q = ed->lineEnd(line); ed->select(q, q); beginChange(); startInsert(c.count, Keys(1, a)); return true; }
		case 'o': case 'O': {
			beginChange();
			std::string ind = indentOf(line);
			Pos q;
			if (a == 'o') { ed->insert(ed->lineEnd(line), ed->eol() + ind); q = ed->lineStart(line + 1) + (Pos)ind.size(); }
			else { Pos ls = ed->lineStart(line); ed->insert(ls, ind + ed->eol()); q = ls + (Pos)ind.size(); }
			ed->select(q, q);
			startInsert(c.count, Keys(1, a));
			return true;
		}
		case 'R': beginChange(); startInsert(c.count, Keys(1, a)); md = REPLACE; ed->setOvertype(true); ed->setCaretStyle(false); return true;
		case 'v': enterVisual(VISUAL); return true;
		case 'V': enterVisual(VLINE); return true;
		case 0x16: enterVisual(VBLOCK); return true;
		case 'm': if (isLower(c.arg) || c.arg == '\'' || c.arg == '`') setMark(c.arg == '`' ? '\'' : c.arg, p); return true;
		case 'q':
			if (recReg) return true;
			if (isLower(c.arg) || isUpper(c.arg) || isDigit(c.arg)) { recReg = c.arg; recBuf.clear(); }
			return true;
		case '@': {
			char32_t r = c.arg == '@' ? lastMacro : c.arg;
			if (!r) return true;
			if (r == ':') { if (!histColon.empty()) for (int i = 0; i < cnt; i++) exCommand(histColon.back()); return true; }
			lastMacro = r;
			Keys keys = ed->decode(getReg(r).text);
			for (int i = 0; i < cnt; i++) runKeys(keys);
			return true;
		}
		case ':': case '/': case '?':
			cmdType = a; cmdFrom = NORMAL; cmdline.clear(); histIdx = -1;
			if (a == ':' && c.count) cmdline = ".,.+" + std::to_string(c.count - 1);
			md = CMDLINE;
			return true;
		case 0x01: addNumber(cnt); return true;
		case 0x18: addNumber(-cnt); return true;
		case 0x04: case 0x15: case 0x06: case 0x02: case 0x05: case 0x19: scroll(a, c.count); return true;
		case '&': {
			if (lastSubPat.empty()) return true;
			std::string arg = "/" + lastSubPat + "/" + lastSubRep + "/";
			substitute(line, line, arg);
			return true;
		}
		}
		return false;
	}
	char32_t b = k[1];
	if (a == 'g') {
		switch (b) {
		case 'J': joinLines(line, c.count, false); return true;
		case 'I': { Pos q = ed->lineStart(line); ed->select(q, q); beginChange(); startInsert(c.count, Keys(1, 'i')); return true; }
		case 'i': { Pos q; if (!getMark('^', q)) q = p; q = std::min(q, ed->length()); ed->select(q, q); beginChange(); startInsert(c.count, Keys(1, 'i')); return true; }
		case 'v':
			if (lastVA < 0) return true;
			vAnchor = std::min(lastVA, ed->length()); vCur = std::min(lastVC, ed->length());
			setMode(lastVisual);
			renderVisual();
			return true;
		case 'p': case 'P': put(c.reg, c.count, b == 'P'); return true;
		}
		return false;
	}
	if (a == 'z') {
		Pos vis = std::max<Pos>(1, ed->visibleLines());
		if (b == 'z' || b == '.') ed->setTopLine(std::max<Pos>(0, line - vis / 2));
		else if (b == 't' || b == K_ENTER) ed->setTopLine(line);
		else if (b == 'b' || b == '-') ed->setTopLine(std::max<Pos>(0, line - vis + 1));
		else return true;
		if (b == '.' || b == K_ENTER || b == '-') moveTo(firstNonBlank(line));
		return true;
	}
	if (a == 'Z') {
		if (b == 'Z') exCommand("x");
		else if (b == 'Q') exCommand("q!");
		return true;
	}
	if (a == ctrlKey('w')) {
		if (b == 'w' || b == 'h' || b == 'j' || b == 'k' || b == 'l' || b == 'p' || b == ctrlKey('w')) host.switchView();
		else if (b == 'q' || b == 'c') exCommand("q");
		else if (b == 'v' || b == 's') exCommand("vs");
		return true;
	}
	return false;
}

void Vim::exec(Cmd& c)
{
	if (!c.op.empty()) {
		int count = (c.count ? c.count : 1) * (c.count2 ? c.count2 : 1);
		if (!c.count && !c.count2) count = 0;
		Range r = opRange(c, count);
		if (!r.ok) return;
		if (c.mot == keysOf("_") && (c.op[0] == '>' || c.op[0] == '<')) r.l2 = std::min(ed->lineOf(ed->caret()) + std::max(count, 1) - 1, ed->lineCount() - 1);
		applyOp(c.op, r, c.reg, 1);
		return;
	}
	if (simple(c)) return;
	if (c.mot[0] == 'q' && c.mot.size() == 1) return;
	Motion m = motion(c, c.count, false);
	if (!m.ok) return;
	if (m.jump) setMark('\'', ed->caret());
	bool vert = c.mot == keysOf("j") || c.mot == keysOf("k") || c.mot == keysOf("gj") || c.mot == keysOf("gk");
	moveTo(m.pos, vert);
	if (c.mot == keysOf("$")) want = INF;
}

void Vim::execVisual(Cmd& c)
{
	int cnt = std::max(c.count, 1);
	if (!c.op.empty() || (c.mot.size() == 1 && Keys(keysOf("xXDCsSYrJuU~pPIAoOcR:")).find(c.mot[0]) != Keys::npos) || c.mot == keysOf("gJ") || c.mot == keysOf("gv")) {
		Keys op = c.op;
		char32_t a = c.mot.empty() ? 0 : c.mot[0];
		if (op.empty()) {
			if (a == 'o') { std::swap(vAnchor, vCur); want = ed->column(vCur); renderVisual(); return; }
			if (a == 'O' && md == VBLOCK) {
				Pos la = ed->lineOf(vAnchor), lc = ed->lineOf(vCur);
				Pos ca = ed->column(vAnchor), cc = ed->column(vCur);
				vAnchor = ed->posAtColumn(la, cc); vCur = ed->posAtColumn(lc, ca);
				renderVisual();
				return;
			}
			if (a == 'O') { std::swap(vAnchor, vCur); renderVisual(); return; }
			if (c.mot == keysOf("gv")) {
				if (lastVA < 0) return;
				std::swap(lastVA, vAnchor); std::swap(lastVC, vCur);
				Mode m = lastVisual; lastVisual = md; setMode(m);
				renderVisual();
				return;
			}
			if (a == ':') {
				exitVisual();
				cmdType = ':'; cmdFrom = NORMAL; cmdline = "'<,'>"; histIdx = -1;
				md = CMDLINE;
				return;
			}
		}
		Range r = visualRange();
		Mode vm = md;
		lastVisual = md; lastVA = vAnchor; lastVC = vCur;
		setMark('<', r.block ? ed->posAtColumn(r.l1, r.c1) : r.s);
		setMark('>', r.e > r.s ? ed->before(r.e) : r.e);
		if (op.empty()) {
			if (a == 'x') op = keysOf("d");
			else if (a == 'X' || a == 'D') { op = keysOf("d"); if (vm == VBLOCK) r.c2 = INF; else r = lineRange(r); }
			else if (a == 'Y') { op = keysOf("y"); if (vm != VBLOCK) r = lineRange(r); }
			else if (a == 's') op = keysOf("c");
			else if (a == 'S' || a == 'R' || a == 'C') { op = keysOf("c"); if (vm == VBLOCK && a == 'C') r.c2 = INF; else r = lineRange(r); }
			else if (a == 'u') op = keysOf("gu");
			else if (a == 'U') op = keysOf("gU");
			else if (a == '~') op = keysOf("g~");
			else if (a == 'c') op = keysOf("c");
		}
		if (!op.empty()) {
			setMode(NORMAL);
			ed->select(ed->caret(), ed->caret());
			md = vm;
			applyOp(op, r, c.reg, cnt);
			if (md == vm) setMode(NORMAL);
			return;
		}
		if (a == 'r') {
			beginChange();
			std::string ch = ed->encode(c.arg);
			auto fill = [&](Pos s, Pos e) {
				std::string t;
				for (Pos q = s; q < e; q = ed->after(q)) {
					int b = ed->byteAt(q);
					if (b == '\r' || b == '\n') t += (char)b;
					else t += ch;
				}
				ed->remove(s, e);
				ed->insert(s, t);
			};
			if (r.block) for (Pos l = r.l1; l <= r.l2; l++) fill(ed->posAtColumn(l, r.c1), r.c2 >= INF ? ed->lineEnd(l) : ed->posAtColumn(l, r.c2));
			else fill(r.s, r.e);
			setMode(NORMAL);
			moveTo(r.block ? ed->posAtColumn(r.l1, r.c1) : r.s);
			endChange();
			return;
		}
		if (a == 'J' || c.mot == keysOf("gJ")) {
			Range lr = lineRange(r);
			setMode(NORMAL);
			joinLines(lr.l1, (int)std::max<Pos>(2, lr.l2 - lr.l1 + 1), a == 'J');
			return;
		}
		if (a == 'p' || a == 'P') {
			Reg saved = getReg(c.reg);
			if (saved.text.empty()) { exitVisual(); return; }
			holdUndo++;
			beginChange();
			setMode(NORMAL);
			md = vm;
			applyOp(keysOf("d"), r, a == 'P' ? '_' : 0, 1);
			setMode(NORMAL);
			const char32_t slot = 0x10FFFF;
			Reg tmp = saved;
			if (vm == VLINE && saved.type != R_LINE) { tmp.text += ed->eol(); tmp.type = R_LINE; }
			regs[slot] = tmp;
			if (vm == VLINE) {
				if (ed->length() == 0) {
					moveTo(0);
					put(slot, cnt, true);
					std::string eol = ed->eol();
					Pos len = ed->length(), el = (Pos)eol.size();
					if (len >= el && ed->text(len - el, len) == eol) ed->remove(len - el, len);
					moveTo(0);
				}
				else if (r.l1 >= ed->lineCount()) { moveTo(ed->lineStart(ed->lineCount() - 1)); put(slot, cnt, false); }
				else { moveTo(ed->lineStart(r.l1)); put(slot, cnt, true); }
			} else {
				Pos q = r.block ? ed->posAtColumn(r.l1, r.c1) : r.s;
				Pos ql = ed->lineOf(q);
				if (q >= ed->lineEnd(ql) && ed->lineEnd(ql) > ed->lineStart(ql)) { Pos b = ed->before(q); ed->select(b, b); put(slot, cnt, false); }
				else { ed->select(q, q); put(slot, cnt, true); }
			}
			regs.erase(slot);
			holdUndo--;
			endChange();
			return;
		}
		if (a == 'I' || a == 'A') {
			setMode(NORMAL);
			if (vm == VBLOCK) {
				beginChange();
				blockIns = r.l2 > r.l1;
				blockL1 = r.l1; blockL2 = r.l2;
				blockEnd = a == 'A' && r.c2 >= INF;
				blockPad = a == 'A';
				blockCol = a == 'I' ? r.c1 : r.c2;
				Pos q;
				if (a == 'A' && !blockEnd) {
					Pos endCol = ed->column(ed->lineEnd(r.l1));
					if (endCol < r.c2) ed->insert(ed->lineEnd(r.l1), std::string((size_t)(r.c2 - endCol), ' '));
					q = ed->posAtColumn(r.l1, r.c2);
				} else if (blockEnd) q = ed->lineEnd(r.l1);
				else q = ed->posAtColumn(r.l1, r.c1);
				ed->select(q, q);
				startInsert(1, Keys());
				return;
			}
			Pos q = a == 'I' ? (vm == VLINE ? firstNonBlank(r.l1) : r.s) : (vm == VLINE ? ed->lineEnd(r.l2) : r.e);
			ed->select(q, q);
			beginChange();
			startInsert(1, Keys());
			return;
		}
		return;
	}
	if (c.mot.size() == 1) {
		char32_t a = c.mot[0];
		if (a == 'v' || a == 'V' || a == 0x16) {
			Mode m = a == 'v' ? VISUAL : a == 'V' ? VLINE : VBLOCK;
			if (md == m) exitVisual();
			else { setMode(m); renderVisual(); }
			return;
		}
		if (a == '/' || a == '?') {
			cmdType = a; cmdFrom = md; cmdline.clear(); histIdx = -1;
			md = CMDLINE;
			return;
		}
		if (a == 0x04 || a == 0x15 || a == 0x06 || a == 0x02 || a == 0x05 || a == 0x19) { scroll(a, c.count); return; }
		if (a == 0x1b) { exitVisual(); return; }
	}
	if (c.mot.size() == 2 && (c.mot[0] == 'i' || c.mot[0] == 'a') && md != VBLOCK) {
		Range r = textObject(c.mot[0], c.mot[1], c.count);
		if (!r.ok) return;
		if (r.lw) { setMode(VLINE); vAnchor = ed->lineStart(r.l1); vCur = ed->lineStart(r.l2); }
		else {
			if (r.e <= r.s) return;
			vAnchor = r.s; vCur = ed->before(r.e);
		}
		want = ed->column(vCur);
		renderVisual();
		return;
	}
	if (c.mot.size() == 2 && c.mot[0] == 'z') { Cmd d = c; d.op.clear(); simple(d); return; }
	Motion m = motion(c, c.count, false);
	if (!m.ok) return;
	bool vert = c.mot == keysOf("j") || c.mot == keysOf("k") || c.mot == keysOf("gj") || c.mot == keysOf("gk");
	moveTo(m.pos, vert);
	if (c.mot == keysOf("$")) { want = INF; renderVisual(); }
}

void Vim::cmdlineKey(char32_t k)
{
	auto& hist = cmdType == ':' ? histColon : histSearch;
	auto leaveCmd = [&]() {
		if (cmdFrom == VISUAL || cmdFrom == VLINE || cmdFrom == VBLOCK) { md = cmdFrom; renderVisual(); }
		else setMode(NORMAL);
	};
	if (k == K_ESC) { leaveCmd(); return; }
	if (k == K_BS) {
		if (cmdline.empty()) { leaveCmd(); return; }
		size_t n = cmdline.size() - 1;
		while (n > 0 && (cmdline[n] & 0xC0) == 0x80) n--;
		cmdline.erase(n);
		return;
	}
	if (k == ctrlKey('u')) { cmdline.clear(); return; }
	if (k == ctrlKey('w')) {
		while (!cmdline.empty() && cmdline.back() == ' ') cmdline.pop_back();
		while (!cmdline.empty() && cmdline.back() != ' ' && cmdline.back() != '/') cmdline.pop_back();
		return;
	}
	if (k == K_UP || k == K_DOWN) {
		if (hist.empty()) return;
		if (k == K_UP) histIdx = histIdx < 0 ? (int)hist.size() - 1 : std::max(0, histIdx - 1);
		else { if (histIdx < 0) return; histIdx++; if (histIdx >= (int)hist.size()) { histIdx = -1; cmdline.clear(); return; } }
		cmdline = hist[histIdx];
		return;
	}
	if (k == K_ENTER) {
		std::string line = cmdline;
		char32_t type = cmdType;
		Mode from = cmdFrom;
		if (!line.empty() && (hist.empty() || hist.back() != line)) hist.push_back(line);
		if (type == ':') {
			setMode(NORMAL);
			exCommand(line);
			return;
		}
		std::string pat = line;
		if (!pat.empty()) lastSearch = pat;
		lastSearchFwd = type == '/';
		if (from == VISUAL || from == VLINE || from == VBLOCK) md = from; else setMode(NORMAL);
		if (lastSearch.empty()) { error("E35: No previous regular expression"); return; }
		Pos s, e;
		Pos cur = md == NORMAL ? ed->caret() : vCur;
		if (search(lastSearch, lastSearchFwd, cur, s, e, true)) {
			if (md == NORMAL) setMark('\'', cur);
			moveTo(s);
		} else if (md != NORMAL) renderVisual();
		return;
	}
	if (k >= 32 && (k < 0xE000 || k > 0xF8FF)) cmdline += utf8(Keys(1, k));
}

bool Vim::exRange(const std::string& s, size_t& i, Pos& line)
{
	Pos n = ed->lineCount();
	bool got = false;
	if (i < s.size()) {
		char c = s[i];
		if (c == '.') { line = curLine(); i++; got = true; }
		else if (c == '$') { line = n - 1; i++; got = true; }
		else if (c >= '0' && c <= '9') {
			Pos v = 0;
			while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
			line = v - 1; got = true;
		} else if (c == '\'' && i + 1 < s.size()) {
			Pos p;
			if (!getMark((char32_t)(unsigned char)s[i + 1], p)) { error("E20: Mark not set"); line = -2; return true; }
			line = ed->lineOf(std::min(p, ed->length())); i += 2; got = true;
		}
	}
	while (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		if (!got) { line = curLine(); got = true; }
		int sign = s[i++] == '+' ? 1 : -1;
		Pos v = 0;
		bool num = false;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i++] - '0'); num = true; }
		line += sign * (num ? v : 1);
	}
	return got;
}

void Vim::substitute(Pos l1, Pos l2, const std::string& arg)
{
	if (arg.empty()) return;
	char d = arg[0];
	std::vector<std::string> parts(1);
	for (size_t i = 1; i < arg.size(); i++) {
		char c = arg[i];
		if (c == '\\' && i + 1 < arg.size() && arg[i + 1] == d && parts.size() < 3) { parts.back() += d; i++; continue; }
		if (c == '\\' && i + 1 < arg.size()) { parts.back() += c; parts.back() += arg[++i]; continue; }
		if (c == d && parts.size() < 3) { parts.emplace_back(); continue; }
		parts.back() += c;
	}
	std::string pat = parts[0];
	std::string rep = parts.size() > 1 ? parts[1] : std::string();
	std::string flags = parts.size() > 2 ? parts[2] : std::string();
	if (pat.empty()) pat = lastSearch;
	if (pat.empty()) { error("E35: No previous regular expression"); return; }
	lastSearch = pat;
	lastSubPat = pat;
	lastSubRep = rep;
	bool global = flags.find('g') != std::string::npos;
	bool icase = false;
	std::string re = toDoc(toRegex(pat, icase));
	rep = toDoc(rep);
	if (flags.find('i') != std::string::npos) icase = true;
	if (flags.find('I') != std::string::npos) icase = false;
	std::string eol = ed->eol();
	int subs = 0;
	Pos lastLine = -1;
	beginChange();
	for (Pos l = l1; l <= l2 && l < ed->lineCount(); l++) {
		Pos from = ed->lineStart(l);
		for (;;) {
			Pos le = ed->lineEnd(l);
			Pos s, e;
			if (from > le || !ed->find(re, from, le, icase, s, e)) break;
			std::string out;
			for (size_t i = 0; i < rep.size(); i++) {
				char c = rep[i];
				if (c == '&') { out += ed->group(0); continue; }
				if (c == '\\' && i + 1 < rep.size()) {
					char n = rep[++i];
					if (n >= '0' && n <= '9') out += ed->group(n - '0');
					else if (n == 'n' || n == 'r') out += eol;
					else if (n == 't') out += '\t';
					else out += n;
					continue;
				}
				out += c;
			}
			Pos before = ed->lineCount();
			ed->remove(s, e);
			ed->insert(s, out);
			subs++;
			Pos added = ed->lineCount() - before;
			l += added;
			l2 += added;
			lastLine = l;
			Pos ne = s + (Pos)out.size();
			if (!global) break;
			from = e == s ? (ne < ed->lineEnd(l) ? ed->after(ne) : ed->lineEnd(l) + 1) : ne;
		}
	}
	if (subs) moveTo(firstNonBlank(lastLine));
	else error("E486: Pattern not found: " + pat);
	closeUndo();
	recordChange = false;
}

void Vim::exCommand(const std::string& raw)
{
	std::string s = trim(raw);
	while (!s.empty() && s[0] == ':') s.erase(0, 1);
	if (s.empty()) return;
	size_t i = 0;
	Pos l1 = curLine(), l2 = l1;
	bool hasRange = false;
	if (s[0] == '%') { l1 = 0; l2 = ed->lineCount() - 1; i = 1; hasRange = true; }
	else {
		Pos a;
		if (exRange(s, i, a)) {
			if (a == -2) return;
			l1 = l2 = a; hasRange = true;
			if (i < s.size() && (s[i] == ',' || s[i] == ';')) {
				i++;
				Pos b;
				if (exRange(s, i, b)) { if (b == -2) return; l2 = b; }
			}
		}
	}
	Pos n = ed->lineCount();
	if (l1 > l2) std::swap(l1, l2);
	while (i < s.size() && s[i] == ' ') i++;
	std::string rest = s.substr(i);
	if (rest.empty()) {
		if (hasRange) { setMark('\'', ed->caret()); moveTo(firstNonBlank(std::max<Pos>(0, std::min(l2, n - 1)))); }
		return;
	}
	if (l1 < 0 || l2 >= n) { error("E16: Invalid range"); return; }
	size_t j = 0;
	if (isalpha((unsigned char)rest[0])) while (j < rest.size() && isalpha((unsigned char)rest[j])) j++;
	else j = 1;
	std::string name = rest.substr(0, j);
	bool bang = j < rest.size() && rest[j] == '!';
	if (bang) j++;
	std::string arg = trim(rest.substr(j));
	auto is = [&](const char* full, size_t minLen) {
		std::string f(full);
		return name.size() >= minLen && name.size() <= f.size() && f.compare(0, name.size(), name) == 0;
	};
	if (name == "s" || is("substitute", 2) || (name.size() == 1 && name[0] == '&')) {
		if (name == "&") arg = "/" + lastSubPat + "/" + lastSubRep + "/" + arg;
		if (!arg.empty() && isalnum((unsigned char)arg[0])) { error("E146: Regular expressions can't be delimited by letters"); return; }
		if (arg.empty()) arg = "/" + lastSubPat + "/" + lastSubRep + "/";
		substitute(l1, l2, arg);
		return;
	}
	if (is("delete", 1) || is("yank", 1)) {
		char32_t reg = 0;
		if (!arg.empty() && !isdigit((unsigned char)arg[0])) reg = (char32_t)(unsigned char)arg[0];
		Range r; r.ok = true; r.lw = true; r.l1 = l1; r.l2 = l2; r.s = ed->lineStart(l1); r.e = ed->lineEnd(l2);
		applyOp(keysOf(name[0] == 'd' ? "d" : "y"), r, reg, 1);
		return;
	}
	if (name == ">" || name == "<") {
		beginChange();
		shiftLines(l1, l2, name == ">" ? 1 : -1, 1);
		moveTo(firstNonBlank(l2));
		endChange();
		return;
	}
	if (is("join", 1)) { joinLines(l1, (int)std::max<Pos>(2, l2 - l1 + 1), !bang); return; }
	if (is("nohlsearch", 3)) return;
	if (is("normal", 4)) {
		Keys k = fromUtf8(arg);
		for (Pos l = l1; l <= l2 && l < ed->lineCount(); l++) {
			moveTo(ed->lineStart(l));
			runKeys(k);
			if (insertLike()) exitInsert();
			if (md != NORMAL && md != CMDLINE) exitVisual();
			pend.clear();
		}
		return;
	}
	if (is("registers", 3) || is("display", 2)) {
		std::string out;
		for (auto& kv : regs) {
			if (kv.first > 0x7F || kv.second.text.empty()) continue;
			std::string t = kv.second.text.substr(0, 20);
			for (char& ch : t) if (ch == '\r' || ch == '\n') ch = ' ';
			out += "\"" + std::string(1, (char)kv.first) + " " + t + "  ";
		}
		error(out.empty() ? "no registers" : out);
		return;
	}
	if (is("marks", 4)) {
		std::string out;
		for (auto& kv : marks[ed->docId()]) if (kv.first < 0x7F) out += std::string(1, (char)kv.first) + ":" + std::to_string(ed->lineOf(std::min(kv.second, ed->length())) + 1) + " ";
		error(out.empty() ? "no marks" : out);
		return;
	}
	if (is("set", 2)) {
		std::string a = arg;
		if (a == "clipboard=unnamed" || a == "clipboard=unnamedplus" || a == "cb=unnamed" || a == "cb=unnamedplus") { clipboardUnnamed = true; host.optionChanged(); return; }
		if (a == "clipboard=" || a == "cb=") { clipboardUnnamed = false; host.optionChanged(); return; }
		if (a == "clipboard?" || a == "cb?") { error(clipboardUnnamed ? "clipboard=unnamed" : "clipboard="); return; }
		error("E518: Unknown option: " + a);
		return;
	}
	if (is("undo", 1)) { ed->undo(); moveTo(clampNormal(ed->caret())); return; }
	if (is("redo", 3)) { ed->redo(); moveTo(clampNormal(ed->caret())); return; }
	std::string err;
	if (!host.ex(*ed, name, arg, bang, err)) {
		if (err.empty()) err = "E492: Not an editor command: " + rest;
		error(err);
	} else if (!err.empty()) error(err);
}
