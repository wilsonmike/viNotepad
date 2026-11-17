#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

typedef intptr_t Pos;
typedef std::u32string Keys;

enum : char32_t {
	K_BS = 8, K_TAB = 9, K_ENTER = 13, K_ESC = 27,
	K_UP = 0xE000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_INS
};

inline char32_t ctrlKey(char c) { return (char32_t)(c & 0x1f); }

enum CaseMode { CASE_UPPER, CASE_LOWER, CASE_TOGGLE };

struct Editor {
	virtual ~Editor() {}
	virtual intptr_t docId() = 0;
	virtual Pos length() = 0;
	virtual int byteAt(Pos p) = 0;
	virtual Pos after(Pos p) = 0;
	virtual Pos before(Pos p) = 0;
	virtual Pos lineCount() = 0;
	virtual Pos lineOf(Pos p) = 0;
	virtual Pos lineStart(Pos line) = 0;
	virtual Pos lineEnd(Pos line) = 0;
	virtual Pos column(Pos p) = 0;
	virtual Pos posAtColumn(Pos line, Pos col) = 0;
	virtual std::string text(Pos a, Pos b) = 0;
	virtual void insert(Pos at, const std::string& s) = 0;
	virtual void remove(Pos a, Pos b) = 0;
	virtual Pos caret() = 0;
	virtual Pos anchor() = 0;
	virtual void select(Pos anchor, Pos caret) = 0;
	virtual void selectBlock(Pos aLine, Pos aCol, Pos cLine, Pos cCol) = 0;
	virtual void beginUndo() = 0;
	virtual void endUndo() = 0;
	virtual void undo() = 0;
	virtual void redo() = 0;
	virtual std::string eol() = 0;
	virtual int indentWidth() = 0;
	virtual int tabWidth() = 0;
	virtual bool useTabs() = 0;
	virtual Pos lineIndent(Pos line) = 0;
	virtual void setLineIndent(Pos line, Pos width) = 0;
	virtual bool find(const std::string& re, Pos from, Pos to, bool icase, Pos& s, Pos& e) = 0;
	virtual std::string group(int n) = 0;
	virtual Pos topLine() = 0;
	virtual void setTopLine(Pos line) = 0;
	virtual Pos visibleLines() = 0;
	virtual std::string encode(char32_t c) = 0;
	virtual Keys decode(const std::string& s) = 0;
	virtual std::string mapCase(const std::string& s, CaseMode m) = 0;
	virtual std::string clipboard() = 0;
	virtual void setClipboard(const std::string& s) = 0;
	virtual void setCaretStyle(bool block) = 0;
	virtual void setOvertype(bool on) = 0;
	virtual void cancelPopups() = 0;
};

struct Host {
	virtual ~Host() {}
	virtual void status(const std::string& s) = 0;
	virtual bool ex(Editor& e, const std::string& cmd, const std::string& arg, bool bang, std::string& err) = 0;
	virtual void switchView() = 0;
	virtual void optionChanged() = 0;
};

enum Mode { NORMAL, INSERT, REPLACE, VISUAL, VLINE, VBLOCK, CMDLINE };

struct Reg {
	std::string text;
	int type = 0;
};

enum { R_CHAR = 0, R_LINE = 1, R_BLOCK = 2 };

class Vim {
public:
	explicit Vim(Host& h) : host(h) {}

	void key(Editor& e, char32_t k);
	void observe(Editor& e, char32_t k);
	void bufferActivated(Editor& e);
	void selectionChanged(Editor& e);
	void textChanged(Editor& e, Pos at, Pos len, bool inserted);
	void enter(Editor& e);
	void leave(Editor& e);

	Mode mode() const { return md; }
	bool insertLike() const { return md == INSERT || md == REPLACE; }
	bool wantsChars() const { return !insertLike() || insPending; }
	std::string statusText() const;

	bool clipboardUnnamed = false;

private:
	struct Cmd {
		char32_t reg = 0;
		int count = 0;
		size_t body = 0;
		Keys op;
		int count2 = 0;
		Keys mot;
		char32_t arg = 0;
	};
	struct Motion {
		Pos pos = 0;
		bool ok = false, lw = false, incl = false, jump = false;
	};
	struct Range {
		Pos s = 0, e = 0;
		bool ok = false, lw = false, block = false;
		Pos l1 = 0, l2 = 0, c1 = 0, c2 = 0;
	};
	enum Parse { MORE, BAD, OK, NONE };

	Host& host;
	Editor* ed = nullptr;
	Mode md = NORMAL;
	Keys pend;
	Pos want = 0;
	Pos vAnchor = 0, vCur = 0;
	Mode lastVisual = VISUAL;
	Pos lastVA = -1, lastVC = -1;
	Pos selA = -1, selC = -1;
	std::string cmdline;
	char32_t cmdType = 0;
	Mode cmdFrom = NORMAL;
	std::vector<std::string> histColon, histSearch;
	int histIdx = -1;
	std::string msg;

	std::map<char32_t, Reg> regs;
	std::string lastClip;
	int lastClipType = 0;

	std::string lastSearch;
	bool lastSearchFwd = true;
	char32_t findCmd = 0, findArg = 0;
	std::string lastSubPat, lastSubRep;
	std::string lastInserted;

	std::map<intptr_t, std::map<char32_t, Pos>> marks;

	bool undoOpen = false;
	intptr_t undoDoc = 0;
	std::vector<intptr_t> staleUndo;
	void settleUndo();

	Keys insKeys;
	int insCount = 1;
	Keys insCmd;
	Pos insStart = 0;
	bool insPending = false;
	bool recordChange = false;
	bool blockIns = false;
	Pos blockL1 = 0, blockL2 = 0, blockCol = 0;
	bool blockPad = false, blockEnd = false;

	struct Change {
		char32_t reg = 0;
		int count = 0;
		Keys body;
		Keys ins;
		bool valid = false;
		Mode vmode = NORMAL;
		Pos vLines = 0, vCols = 0;
	} lastChange, curChange;
	bool replaying = false;
	int holdUndo = 0;
	int runDepth = 0;

	char32_t recReg = 0;
	Keys recBuf;
	char32_t lastMacro = 0;

	void feed(char32_t k);
	void normalKey(char32_t k);
	void insertKey(char32_t k);
	void cmdlineKey(char32_t k);

	Parse parse(const Keys& k, Cmd& c, bool visual);
	Parse parseMotion(const Keys& k, size_t& i, Cmd& c, bool allowObj);
	void exec(Cmd& c);
	void execVisual(Cmd& c);
	bool simple(Cmd& c);

	Motion motion(const Cmd& c, int count, bool forOp);
	Range textObject(char32_t ia, char32_t obj, int count);
	Range opRange(const Cmd& c, int count);
	Range visualRange();
	void applyOp(const Keys& op, Range r, char32_t reg, int count);

	void beginChange();
	void endChange();
	void closeUndo();
	void startInsert(int count, const Keys& cmd);
	void exitInsert();
	void enterVisual(Mode m);
	void exitVisual();
	void renderVisual();
	void setMode(Mode m);

	void moveTo(Pos p, bool keepWant = false);
	Pos clampNormal(Pos p);
	Pos firstNonBlank(Pos line);
	Pos lineLastChar(Pos line);
	Pos curLine();
	int cls(Pos p, bool big);
	bool isBlankLine(Pos line);
	Pos wordFwd(Pos p, bool big, bool stopEol);
	Pos wordEnd(Pos p, bool big);
	Pos wordBack(Pos p, bool big);
	Pos wordEndBack(Pos p, bool big);
	Pos findChar(Pos p, char32_t cmd, char32_t c, int count, bool repeat);
	Pos matchPair(Pos p);
	Pos paragraph(Pos p, int dir, int count);
	bool findBack(const std::string& re, Pos from, Pos to, bool icase, Pos& s, Pos& e);
	Range lineRange(const Range& r);
	std::string wordUnder(Pos p, Pos& s);
	std::string indentOf(Pos line);

	bool search(const std::string& vpat, bool fwd, Pos from, Pos& s, Pos& e, bool wrapMsg);
	static std::string toRegex(const std::string& vpat, bool& icase);
	std::string toDoc(const std::string& u);

	void setReg(char32_t r, const std::string& text, int type, bool del);
	Reg getReg(char32_t r);
	void put(char32_t reg, int count, bool before, bool keepUnnamed = false);
	void joinLines(Pos line, int count, bool spaces);
	void replaceChars(char32_t c, int count);
	void addNumber(int delta);
	void scroll(char32_t k, int count);
	void shiftLines(Pos l1, Pos l2, int dir, int times);

	void runKeys(const Keys& k);
	void dot(int count);
	void exCommand(const std::string& line);
	bool exRange(const std::string& s, size_t& i, Pos& line);
	void substitute(Pos l1, Pos l2, const std::string& arg);
	void setMark(char32_t m, Pos p);
	bool getMark(char32_t m, Pos& p);
	void error(const std::string& m) { msg = m; }
	void syncSel();
};
