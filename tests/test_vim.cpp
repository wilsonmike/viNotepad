#include "../src/Vim.h"

#include <cstdio>
#include <regex>

struct Fake : Editor {
	std::string t;
	std::string nl = "\n";
	Pos car = 0, anc = 0;
	int depth = 0;
	std::vector<std::pair<std::string, Pos>> undos, redos;
	std::string clip;
	std::smatch m;
	std::string hay;
	bool block = true;

	intptr_t docId() override { return 1; }
	Pos length() override { return (Pos)t.size(); }
	int byteAt(Pos p) override { return p >= 0 && p < length() ? (unsigned char)t[p] : 0; }
	Pos after(Pos p) override
	{
		if (p >= length()) return length();
		if (t[p] == '\r' && p + 1 < length() && t[p + 1] == '\n') return p + 2;
		p++;
		while (p < length() && ((unsigned char)t[p] & 0xC0) == 0x80) p++;
		return p;
	}
	Pos before(Pos p) override
	{
		if (p <= 0) return 0;
		if (p >= 2 && t[p - 1] == '\n' && t[p - 2] == '\r') return p - 2;
		p--;
		while (p > 0 && ((unsigned char)t[p] & 0xC0) == 0x80) p--;
		return p;
	}
	std::vector<Pos> starts()
	{
		std::vector<Pos> s{ 0 };
		for (Pos i = 0; i < length(); i++) {
			if (t[i] == '\r' && i + 1 < length() && t[i + 1] == '\n') { s.push_back(i + 2); i++; }
			else if (t[i] == '\n' || t[i] == '\r') s.push_back(i + 1);
		}
		return s;
	}
	Pos lineCount() override { return (Pos)starts().size(); }
	Pos lineOf(Pos p) override
	{
		auto s = starts();
		Pos l = 0;
		for (size_t i = 0; i < s.size(); i++) if (s[i] <= p) l = (Pos)i;
		return l;
	}
	Pos lineStart(Pos line) override
	{
		auto s = starts();
		if (line < 0) return 0;
		if (line >= (Pos)s.size()) return length();
		return s[line];
	}
	Pos lineEnd(Pos line) override
	{
		Pos p = lineStart(line);
		while (p < length() && t[p] != '\r' && t[p] != '\n') p++;
		return p;
	}
	Pos column(Pos p) override
	{
		Pos q = lineStart(lineOf(p)), c = 0;
		while (q < p) { c = t[q] == '\t' ? (c / 4 + 1) * 4 : c + 1; q = after(q); }
		return c;
	}
	Pos posAtColumn(Pos line, Pos col) override
	{
		Pos q = lineStart(line), e = lineEnd(line), c = 0;
		while (q < e) {
			Pos nc = t[q] == '\t' ? (c / 4 + 1) * 4 : c + 1;
			if (nc > col) break;
			c = nc;
			q = after(q);
		}
		return q;
	}
	std::string text(Pos a, Pos b) override
	{
		a = std::max<Pos>(0, std::min(a, length()));
		b = std::max(a, std::min(b, length()));
		return t.substr(a, b - a);
	}
	void snap()
	{
		if (depth == 0) { undos.push_back({ t, car }); redos.clear(); }
	}
	Vim* vim = nullptr;
	void insert(Pos at, const std::string& s) override { snap(); t.insert(at, s); if (vim) vim->textChanged(*this, at, (Pos)s.size(), true); }
	void remove(Pos a, Pos b) override { if (b <= a) return; snap(); t.erase(a, b - a); if (vim) vim->textChanged(*this, a, b - a, false); }
	Pos caret() override { return car; }
	Pos anchor() override { return anc; }
	void select(Pos a, Pos c) override { anc = a; car = c; }
	void selectBlock(Pos, Pos, Pos, Pos) override {}
	void beginUndo() override { if (depth++ == 0) { undos.push_back({ t, car }); redos.clear(); } }
	void endUndo() override { if (--depth == 0 && undos.back().first == t) undos.pop_back(); }
	void undo() override
	{
		if (undos.empty()) return;
		redos.push_back({ t, car });
		auto u = undos.back();
		undos.pop_back();
		Pos d = 0;
		while (d < (Pos)t.size() && d < (Pos)u.first.size() && t[d] == u.first[d]) d++;
		t = u.first;
		car = anc = std::min<Pos>(d, length());
	}
	void redo() override
	{
		if (redos.empty()) return;
		undos.push_back({ t, car });
		t = redos.back().first;
		redos.pop_back();
	}
	std::string eol() override { return nl; }
	int indentWidth() override { return 4; }
	int tabWidth() override { return 4; }
	bool useTabs() override { return false; }
	Pos lineIndent(Pos line) override { Pos p = lineStart(line); Pos e = lineEnd(line); while (p < e && (t[p] == ' ' || t[p] == '\t')) p++; return column(p); }
	void setLineIndent(Pos line, Pos w) override
	{
		Pos s = lineStart(line), p = s, e = lineEnd(line);
		while (p < e && (t[p] == ' ' || t[p] == '\t')) p++;
		remove(s, p);
		insert(s, std::string((size_t)w, ' '));
	}
	bool find(const std::string& re, Pos from, Pos to, bool icase, Pos& s, Pos& e) override
	{
		auto fl = std::regex::ECMAScript;
		if (icase) fl |= std::regex::icase;
		std::regex r(re, fl);
		auto mf = std::regex_constants::match_default;
		if (from > 0) mf |= std::regex_constants::match_prev_avail;
		if (from != lineStart(lineOf(from))) mf |= std::regex_constants::match_not_bol;
		if (to != lineEnd(lineOf(to))) mf |= std::regex_constants::match_not_eol;
		hay = t;
		if (!std::regex_search(hay.cbegin() + from, hay.cbegin() + to, m, r, mf)) return false;
		s = from + m.position(0);
		e = s + m.length(0);
		return true;
	}
	std::string group(int n) override { return n < (int)m.size() ? m.str(n) : std::string(); }
	Pos topLine() override { return 0; }
	void setTopLine(Pos) override {}
	Pos visibleLines() override { return 20; }
	std::string encode(char32_t c) override
	{
		std::string s;
		if (c < 0x80) s += (char)c;
		else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
		else { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
		return s;
	}
	Keys decode(const std::string& s) override
	{
		Keys k;
		for (size_t i = 0; i < s.size();) {
			unsigned char b = s[i];
			if (b < 0x80) { k += b; i++; }
			else if ((b & 0xE0) == 0xC0) { k += (char32_t)(((b & 0x1F) << 6) | (s[i + 1] & 0x3F)); i += 2; }
			else { k += (char32_t)(((b & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F)); i += 3; }
		}
		return k;
	}
	std::string mapCase(const std::string& s, CaseMode md) override
	{
		std::string o = s;
		for (char& c : o) {
			if (md == CASE_UPPER) c = (char)toupper((unsigned char)c);
			else if (md == CASE_LOWER) c = (char)tolower((unsigned char)c);
			else c = isupper((unsigned char)c) ? (char)tolower((unsigned char)c) : (char)toupper((unsigned char)c);
		}
		return o;
	}
	std::string clipboard() override { return clip; }
	void setClipboard(const std::string& s) override { clip = s; }
	void setCaretStyle(bool b) override { block = b; }
	void setOvertype(bool) override {}
	void cancelPopups() override {}
};

struct FakeHost : Host {
	std::string last, exLog;
	void status(const std::string& s) override { last = s; }
	bool ex(Editor&, const std::string& cmd, const std::string& arg, bool bang, std::string&) override
	{
		exLog += cmd + (bang ? "!" : "") + (arg.empty() ? "" : " " + arg) + ";";
		return cmd == "w" || cmd == "q" || cmd == "x" || cmd == "wq";
	}
	void switchView() override { exLog += "view;"; }
	void optionChanged() override {}
};

static Keys parseKeys(const std::string& s)
{
	Keys k;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '<') {
			size_t e = s.find('>', i);
			if (e != std::string::npos && e > i + 1) {
				std::string n = s.substr(i + 1, e - i - 1);
				char32_t v = 0;
				if (n == "Esc") v = K_ESC;
				else if (n == "CR") v = K_ENTER;
				else if (n == "BS") v = K_BS;
				else if (n == "Tab") v = K_TAB;
				else if (n == "Up") v = K_UP;
				else if (n == "Down") v = K_DOWN;
				else if (n == "lt") v = '<';
				else if (n.size() == 3 && n[0] == 'C' && n[1] == '-') v = ctrlKey(n[2]);
				if (v) { k += v; i = e; continue; }
			}
		}
		unsigned char b = (unsigned char)s[i];
		if (b >= 0xC0 && b < 0xE0 && i + 1 < s.size()) { k += (char32_t)(((b & 0x1F) << 6) | (s[i + 1] & 0x3F)); i++; continue; }
		k += b;
	}
	return k;
}

static int fails = 0, total = 0;

struct Session {
	Fake e;
	FakeHost h;
	Vim v{ h };
	explicit Session(const std::string& start, const std::string& nl = "\n")
	{
		e.nl = nl;
		e.vim = &v;
		std::string s = start;
		size_t c = s.find('|');
		if (c != std::string::npos) s.erase(c, 1); else c = 0;
		e.t = s;
		e.car = e.anc = (Pos)c;
		v.enter(e);
	}
	void keys(const std::string& k)
	{
		for (char32_t c : parseKeys(k)) {
			if (v.insertLike() && c != K_ESC && c != ctrlKey('r') && c != ctrlKey('w') && c != ctrlKey('u') && !v.wantsChars()) {
				std::string s = e.encode(c);
				if (c == K_BS) { Pos p = e.before(e.car); e.t.erase(p, e.car - p); v.textChanged(e, p, e.car - p, false); e.car = e.anc = p; }
				else if (c == K_ENTER) { e.snap(); e.t.insert(e.car, e.nl); v.textChanged(e, e.car, (Pos)e.nl.size(), true); e.car += (Pos)e.nl.size(); e.anc = e.car; }
				else {
					if (e.depth == 0) e.snap();
					if (v.mode() == REPLACE && e.car < e.lineEnd(e.lineOf(e.car))) e.t.erase(e.car, 1);
					e.t.insert(e.car, s); v.textChanged(e, e.car, (Pos)s.size(), true); e.car += (Pos)s.size(); e.anc = e.car;
				}
				v.observe(e, c);
			} else v.key(e, c);
		}
	}
	std::string shown()
	{
		std::string s = e.t;
		s.insert((size_t)e.car, "|");
		return s;
	}
};

static void check(const char* name, const std::string& start, const std::string& k, const std::string& want, const std::string& nl = "\n")
{
	total++;
	Session s(start, nl);
	s.keys(k);
	std::string got = s.shown();
	if (got != want) {
		fails++;
		std::printf("FAIL %s\n  keys: %s\n  want: [%s]\n  got:  [%s]\n", name, k.c_str(), want.c_str(), got.c_str());
	}
}

int main()
{
	check("h", "ab|c", "h", "a|bc");
	check("l stops at last char", "a|bc", "lll", "ab|c");
	check("j k keep column", "abc|d\nx\nabcdef", "jj", "abcd\nx\nabc|def");
	check("j from short line", "abc|d\nx\nabcdef", "jk", "abc|d\nx\nabcdef");
	check("w", "|foo bar.baz", "w", "foo |bar.baz");
	check("w punct", "|foo bar.baz", "ww", "foo bar|.baz");
	check("W", "|foo bar.baz qux", "WW", "foo bar.baz |qux");
	check("w across lines", "foo |bar\n  baz", "w", "foo bar\n  |baz");
	check("w stops on empty line", "|foo\n\nbar", "w", "foo\n|\nbar");
	check("b", "foo bar |baz", "b", "foo |bar baz");
	check("b across lines", "foo\n|bar", "b", "|foo\nbar");
	check("e", "|foo bar", "e", "fo|o bar");
	check("e twice", "|foo bar", "ee", "foo ba|r");
	check("ge", "foo ba|r", "ge", "fo|o bar");
	check("0 ^ $", "  ab|cd", "0", "|  abcd");
	check("^", "  ab|cd", "^", "  |abcd");
	check("$", "  |abcd", "$", "  abc|d");
	check("gg", "a\nb\n|c", "gg", "|a\nb\nc");
	check("G", "|a\nb\n  c", "G", "a\nb\n  |c");
	check("3G", "|a\nb\nc\nd", "3G", "a\nb\n|c\nd");
	check("f", "|abcabc", "fc", "ab|cabc");
	check("f ;", "|abcabc", "fc;", "abcab|c");
	check("t", "|abcabc", "tc", "a|bcabc");
	check("t ;", "|abcabc", "tc;", "abca|bc");
	check("f ; ,", "|abcabc", "fc;,", "ab|cabc");
	check("%", "|(a [b] c)", "%", "(a [b] c|)");
	check("% back", "(a [b] c|)", "%", "|(a [b] c)");
	check("}", "|a\nb\n\nc", "}", "a\nb\n|\nc");
	check("{", "a\n\nb\n|c", "{", "a\n|\nb\nc");

	check("x", "a|bc", "x", "a|c");
	check("x last char", "ab|c", "x", "a|b");
	check("3x", "|abcde", "3x", "|de");
	check("X", "ab|c", "X", "a|c");
	check("dw", "|foo bar", "dw", "|bar");
	check("dw last word", "foo |bar\nbaz", "dw", "foo| \nbaz");
	check("d2w", "|a b c d", "d2w", "|c d");
	check("2dw", "|a b c d", "2dw", "|c d");
	check("de", "|foo bar", "de", "| bar");
	check("db", "foo |bar", "db", "|bar");
	check("d$", "fo|o bar", "d$", "f|o");
	check("D", "fo|o bar", "D", "f|o");
	check("d0", "foo b|ar", "d0", "|ar");
	check("dd", "a\n|b\nc", "dd", "a\n|c");
	check("dd last", "a\nb\n|c", "dd", "a\n|b");
	check("dd only", "|abc", "dd", "|");
	check("3dd", "|a\nb\nc\nd", "3dd", "|d");
	check("dj", "|a\nb\nc", "dj", "|c");
	check("dk", "a\nb\n|c", "dk", "|a");
	check("dG", "a\n|b\nc", "dG", "|a");
	check("dgg", "a\nb\n|c\nd", "dgg", "|d");
	check("dfx", "|abxcd", "dfx", "|cd");
	check("dtx", "|abxcd", "dtx", "|xcd");
	check("d}", "|a\nb\n\nc", "d}", "|\nc");
	check("diw", "foo b|ar baz", "diw", "foo | baz");
	check("daw", "foo b|ar baz", "daw", "foo |baz");
	check("daw last", "foo b|ar", "daw", "fo|o");
	check("di\"", "say \"he|llo\" now", "di\"", "say \"|\" now");
	check("da\"", "say \"he|llo\" now", "da\"", "say |now");
	check("ci\" before quotes", "|x = \"abc\";", "ci\"q<Esc>", "x = \"|q\";");
	check("di(", "f(a, |b)", "di(", "f(|)");
	check("da(", "f(a, |b)", "da(", "|f");
	check("dib nested", "f(a, (|b), c)", "dib", "f(a, (|), c)");
	check("di{ multiline", "if {\n    fo|o;\n    bar;\n}", "di{", "if {\n|}");
	check("ci{ multiline", "if {\n    fo|o;\n}", "ci{x<Esc>", "if {\n    |x\n}");
	check("dip", "a\n|b\n\nc", "dip", "|\nc");
	check("dap", "a\n|b\n\nc", "dap", "|c");

	check("cw", "|foo bar", "cwxy<Esc>", "x|y bar");
	check("cw on last char", "fo|o bar", "cwx<Esc>", "fo|x bar");
	check("cc keeps indent", "  a|bc\nd", "ccx<Esc>", "  |x\nd");
	check("C", "ab|cd", "Cx<Esc>", "ab|x");
	check("s", "a|bc", "sxy<Esc>", "ax|yc");
	check("S", "  a|bc", "Sz<Esc>", "  |z");
	check("c$", "a|bc", "c$Z<Esc>", "a|Z");

	check("i", "a|bc", "iXY<Esc>", "aX|Ybc");
	check("a", "a|bc", "aXY<Esc>", "abX|Yc");
	check("I", "  a|bc", "IX<Esc>", "  |Xabc");
	check("A", "a|bc", "AX<Esc>", "abc|X");
	check("o", "  a|bc\nd", "ox<Esc>", "  abc\n  |x\nd");
	check("O", "  a|bc", "Ox<Esc>", "  |x\n  abc");
	check("3i", "|z", "3iab<Esc>", "ababa|bz");
	check("esc at line start", "|abc", "i<Esc>", "|abc");
	check("insert enter", "|ab", "ix<CR>y<Esc>", "x\n|yab");
	check("insert BS", "a|b", "ixy<BS><Esc>", "a|xb");

	check("yy p", "|a\nb", "yyp", "a\n|a\nb");
	check("yy P", "a\n|b", "yyP", "a\n|b\nb");
	check("yy p last", "a\n|b", "yyp", "a\nb\n|b");
	check("yw p", "|foo bar", "ywP", "foo| foo bar");
	check("dd p", "|a\nb\nc", "ddp", "b\n|a\nc");
	check("x p swap", "|ab", "xp", "b|a");
	check("3p", "|a", "yl3p", "aaa|a");
	check("named register", "|foo\nbar", "\"ayyj\"byy\"ap", "foo\nbar\n|foo");
	check("append register", "|foo\nbar", "\"ayyj\"Ayy\"ap", "foo\nbar\n|foo\nbar");
	check("yank keeps 0", "|a\nb", "yyjdd\"0p", "a\n|a");
	check("blackhole", "|a\nb", "yyj\"_ddp", "a\n|a");
	check("yiw", "foo b|ar", "yiw0P", "ba|rfoo bar");

	check("u", "|abc", "xu", "|abc");
	check("u insert group", "|abc", "cwxyz<Esc>u", "|abc");
	check("C-r", "|abc", "xu<C-r>", "|bc");
	check("u dd", "a\n|b\nc", "ddu", "a\n|b\nc");

	check(". x", "|abcd", "x..", "|d");
	check(". dw", "|a b c d", "dw.", "|c d");
	check(". cw", "|foo bar baz", "cwX<Esc>w.", "X |X baz");
	check(". A", "|a\nb", "A;<Esc>j.", "a;\nb|;");
	check(". o", "|a", "ox<Esc>.", "a\nx\n|x");
	check(". with count", "|abcdef", "x3.", "|ef");
	check(". dd", "|a\nb\nc\nd", "dd.", "|c\nd");
	check(". 3i", "|z", "2ia<Esc>.", "aa|aaz");

	check("J", "|a\n  b", "J", "a| b");
	check("3J", "|a\nb\nc", "3J", "a b| c");
	check("gJ", "|a\n  b", "gJ", "a|  b");
	check("J empty", "|a\n\nb", "J", "|a\nb");
	check("r", "|abc", "rx", "|xbc");
	check("3r", "|abcd", "3rx", "xx|xd");
	check("~", "|abc", "~~", "AB|c");
	check("gUw", "|foo bar", "gUw", "|FOO bar");
	check("guu", "|FOO Bar", "guu", "|foo bar");
	check("g~iw", "x |foo", "g~iw", "x |FOO");
	check(">>", "|a\nb", ">>", "    |a\nb");
	check("2>>", "|a\nb", "2>>", "    |a\n    b");
	check("<<", "      |a", "<<", "    |a");
	check(">j", "|a\nb\nc", ">j", "    |a\n    b\nc");
	check("C-a", "x |9 y", "<C-a>", "x 1|0 y");
	check("C-a ahead", "|x 41 y", "<C-a>", "x 4|2 y");
	check("C-x neg", "|x 0", "<C-x>", "x -|1");
	check("5C-a", "|7", "5<C-a>", "1|2");

	check("v d", "a|bcd", "vld", "a|d");
	check("v $ d", "a|bc\nd", "v$d", "a|d");
	check("v y P", "|abc", "vlyP", "a|babc");
	check("V d", "a\n|b\nc", "Vd", "a\n|c");
	check("V j d", "|a\nb\nc", "Vjd", "|c");
	check("V >", "|a\nb", "Vj>", "    |a\n    b");
	check("v iw", "foo b|ar baz", "viwd", "foo | baz");
	check("v o", "a|bcd", "vlohd", "|d");
	check("v ~", "|abc", "vl~", "|ABc");
	check("v U", "|abc", "vlU", "|ABc");
	check("v r", "|abc", "vlrx", "|xxc");
	check("v p", "|foo bar", "yiwwviwp", "foo fo|o");
	check("V p", "|a\nb", "yyjVp", "a\n|a");
	check("v c", "|abc", "vlcX<Esc>", "|Xc");
	check("v J", "|a\nb\nc", "VjjJ", "a b| c");
	check("v esc", "|abc", "vl<Esc>", "a|bc");
	check("gv", "|abc", "vl<Esc>0gvd", "|c");
	check("C-v d", "|abc\nabc\nabc", "<C-v>jld", "|c\nc\nabc");
	check("C-v I", "|abc\nabc", "<C-v>jIX<Esc>", "|Xabc\nXabc");
	check("C-v A", "|abc\nabc", "<C-v>jAX<Esc>", "a|Xbc\naXbc");
	check("C-v $A", "|ab\nabcd", "<C-v>j$AX<Esc>", "ab|X\nabcdX");
	check("C-v c", "|abc\nabc", "<C-v>jlcZ<Esc>", "|Zc\nZc");
	check("C-v y p", "|ab\ncd", "<C-v>jy$p", "ab|a\ncdc");

	check("/ search", "|foo bar foo", "/foo<CR>", "foo bar |foo");
	check("/ wrap", "foo bar |foo", "/foo<CR>", "|foo bar foo");
	check("? search", "foo bar |foo", "?bar<CR>", "foo |bar foo");
	check("n N", "|a x a x a", "/a<CR>nN", "a x |a x a");
	check("*", "|foo bar foo", "*", "foo bar |foo");
	check("* whole word", "|foo foobar foo", "*", "foo foobar |foo");
	check("#", "foo bar |foo", "#", "|foo bar foo");
	check("/ vim group", "|a1 b2", "/\\(b\\)2<CR>", "a1 |b2");
	check("/ \\< \\>", "|xfoo foo", "/\\<foo\\><CR>", "xfoo |foo");
	check("d/", "|abc def", "/def<CR>", "abc |def");
	check("/ in visual", "|abc def", "v/d<CR>d", "|ef");

	check(":s", "|foo foo", ":s/foo/bar/<CR>", "|bar foo");
	check(":s g", "|foo foo", ":s/foo/bar/g<CR>", "|bar bar");
	check(":%s", "|a\nb a\na", ":%s/a/x/g<CR>", "x\nb x\n|x");
	check(":s group", "|ab", ":s/\\(a\\)\\(b\\)/\\2\\1/<CR>", "|ba");
	check(":s &", "|ab", ":s/a/[&]/<CR>", "|[a]b");
	check(":s newline", "|a,b", ":s/,/\\r/<CR>", "a\n|b");
	check(":range s", "|a\na\na", ":2,3s/a/b/<CR>", "a\nb\n|b");
	check(":'<,'>s", "|a\na\na", "Vj:s/a/b/<CR>", "b\n|b\na");
	check(":s icase", "|Foo", ":s/foo/x/i<CR>", "|x");
	check(":s empty pat", "|foo bar", "/bar<CR>:s//X/<CR>", "|foo X");
	check(":N", "|a\nb\nc", ":3<CR>", "a\nb\n|c");
	check(":$", "|a\nb\nc", ":$<CR>", "a\nb\n|c");
	check(":d", "|a\nb\nc", ":2d<CR>", "a\n|c");
	check(":1,2d", "a\nb\n|c", ":1,2d<CR>", "|c");
	check(":j", "|a\nb", ":j<CR>", "a| b");
	check(":>", "|a", ":><CR>", "    |a");
	check(":normal", "|a\nb", ":%norm Ax<CR>", "ax\nb|x");
	check(":s/\\d\\+", "|a12b", ":s/\\d\\+/N/<CR>", "|aNb");
	check("cmdline BS cancel", "|ab", ":<BS>x", "|b");
	check("cmdline esc", "|ab", ":foo<Esc>x", "|b");
	check("& repeat", "|aa\naa", ":s/a/b/<CR>j&", "ba\n|ba");

	check("marks", "|a\nb\nc", "majj'ax", "|\nb\nc");
	check("mark backtick", "a|bc\nd", "mbj`bx", "a|c\nd");
	check("''", "|a\nb\nc", "G''", "|a\nb\nc");

	check("macro", "|a\nb\nc", "qqA!<Esc>jq2@q", "a!\nb!\nc|!");
	check("macro @@", "|1\n1\n1", "qq<C-a>jq@q@@", "2\n2\n|2");
	check("macro with insert", "|a\nb", "qwIx<Esc>jq@w", "xa\n|xb");

	check("crlf dd", "a\r\n|b\r\nc", "dd", "a\r\n|c", "\r\n");
	check("crlf j", "a|b\r\ncd", "j", "ab\r\nc|d", "\r\n");
	check("crlf o", "|a\r\nb", "ox<Esc>", "a\r\n|x\r\nb", "\r\n");
	check("crlf yyp last", "a\r\n|b", "yyp", "a\r\nb\r\n|b", "\r\n");
	check("crlf $", "|ab\r\ncd", "$", "a|b\r\ncd", "\r\n");
	check("crlf J", "|a\r\nb", "J", "a| b", "\r\n");
	check("crlf w", "a|b\r\ncd", "w", "ab\r\n|cd", "\r\n");

	check("utf8 l", "|\xc3\xa9t\xc3\xa9", "l", "\xc3\xa9|t\xc3\xa9");
	check("utf8 x", "|\xc3\xa9t", "x", "|t");
	check("utf8 f", "|ab\xc3\xa9", "f\xc3\xa9", "ab|\xc3\xa9");

	check("count count", "|a b c d e f g", "2d2w", "|e f g");
	check("esc cancels pending", "|abc", "d<Esc>x", "|bc");
	check("bad cmd ignored", "|abc", "dzx", "|bc");
	check("arrows", "|ab\ncd", "<Down>", "ab\n|cd");
	check("insert C-w", "foo bar|", "a<C-w><Esc>", "foo| ");
	check("insert C-r", "|foo", "yiwA <C-r>\"<Esc>", "foo fo|o");
	check("p after empty line", "|\nx", "yyp", "\n|\nx");
	check("ZZ", "|a", "ZZ", "|a");
	check("unknown g ignored", "|ab", "gox", "|b");
	check("unknown z ignored", "|ab", "zqx", "|b");

	{
		total++;
		Session s("|abc");
		s.keys(":set clipboard=unnamed<CR>yiw");
		if (s.e.clip != "abc") { fails++; std::printf("FAIL clipboard unnamed: [%s]\n", s.e.clip.c_str()); }
		total++;
		s.e.clip = "zz";
		s.keys("P");
		if (s.shown() != "z|zabc") { fails++; std::printf("FAIL clipboard paste: [%s]\n", s.shown().c_str()); }
	}
	{
		total++;
		Session s("|abc");
		s.keys(":w<CR>:wq<CR>ZZ:xyz<CR>");
		if (s.h.exLog != "w;wq;x;xyz;" || s.h.last.find("E492") == std::string::npos) { fails++; std::printf("FAIL ex host: [%s] [%s]\n", s.h.exLog.c_str(), s.h.last.c_str()); }
	}
	{
		total++;
		Session s("|abc");
		s.keys("2d");
		if (s.h.last.find("2d") == std::string::npos) { fails++; std::printf("FAIL pending status: [%s]\n", s.h.last.c_str()); }
		total++;
		s.keys("<Esc>i");
		if (s.h.last.find("INSERT") == std::string::npos || s.e.block) { fails++; std::printf("FAIL insert status: [%s]\n", s.h.last.c_str()); }
	}
	{
		total++;
		Session s("ab|c");
		s.e.car = 3; s.e.anc = 3;
		s.v.selectionChanged(s.e);
		if (s.e.car != 2) { fails++; std::printf("FAIL clamp after click: %d\n", (int)s.e.car); }
		total++;
		s.e.anc = 0; s.e.car = 2;
		s.v.selectionChanged(s.e);
		if (s.v.mode() != VISUAL) { fails++; std::printf("FAIL mouse selection -> visual\n"); }
		s.keys("d");
		total++;
		if (s.shown() != "|c") { fails++; std::printf("FAIL mouse visual d: [%s]\n", s.shown().c_str()); }
	}

	check("visual d dot", "|abcdefgh", "xvld.", "|fgh");
	check("visual c dot", "|foo bar", "viwcX<Esc>w.", "X |X");
	check("block I dot", "|abc\nabc\nxyz", "<C-v>jIX<Esc>j.", "Xabc\n|XXabc\nXxyz");
	check("v multiline d dot", "|abc\ndef\nghi\njkl\nmno", "vjd.", "|hi\njkl\nmno");
	check("visual ~ dot", "|abcd", "vl~ll.", "AB|CD");
	check("visual y does not set dot", "|abc", "xvly.", "|c");
	check("V d dot","|a\nb\nc\nd\ne", "Vjd.", "|e");
	check("count dot replaces count", "|a b c d e f g h", "d3w2.", "|f g h");
	check("dot keeps inner count", "|a b c d e f g h", "d3w.", "|g h");
	check("mark follows deleted line above", "aaa\nbbb\n|ccc", "maggdd`a", "bbb\n|ccc");
	check("mark follows insert above", "aaa\n|bbb", "maggOx<Esc>`a", "x\naaa\n|bbb");
	check("J keeps trailing space", "|a  \nb", "J", "a  |b");
	check("ge stops on empty line", "foo\n\nba|r", "ge", "foo\n|\nbar");
	check("Vp whole buffer", "|abc", "yyVp", "|abc");
	check("dot register", "|", "ifoo<Esc>a<C-r>.<Esc>", "foofo|o");
	{
		total++;
		Session s("|ab", "\r\n");
		s.e.clip = "x\ny";
		s.keys("\"+p");
		if (s.shown() != "a|x\r\nyb") { fails++; std::printf("FAIL put normalizes line endings: [%s]\n", s.shown().c_str()); }
	}

	std::printf("%d/%d passed\n", total - fails, total);
	return fails ? 1 : 0;
}
