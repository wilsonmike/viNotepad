#pragma once

#include <windows.h>

#include "Scintilla.h"
#include "Vim.h"

extern int sciInsertCaretStyle;

std::wstring toWide(const std::string& s, UINT cp);
std::string fromWide(const std::wstring& w, UINT cp);

class SciEditor : public Editor {
public:
	explicit SciEditor(HWND h);
	HWND hwnd;

	intptr_t docId() override;
	Pos length() override;
	int byteAt(Pos p) override;
	Pos after(Pos p) override;
	Pos before(Pos p) override;
	Pos lineCount() override;
	Pos lineOf(Pos p) override;
	Pos lineStart(Pos line) override;
	Pos lineEnd(Pos line) override;
	Pos column(Pos p) override;
	Pos posAtColumn(Pos line, Pos col) override;
	std::string text(Pos a, Pos b) override;
	void insert(Pos at, const std::string& s) override;
	void remove(Pos a, Pos b) override;
	Pos caret() override;
	Pos anchor() override;
	void select(Pos anchor, Pos caret) override;
	void selectBlock(Pos aLine, Pos aCol, Pos cLine, Pos cCol) override;
	void beginUndo() override;
	void endUndo() override;
	void undo() override;
	void redo() override;
	std::string eol() override;
	int indentWidth() override;
	int tabWidth() override;
	bool useTabs() override;
	Pos lineIndent(Pos line) override;
	void setLineIndent(Pos line, Pos width) override;
	bool find(const std::string& re, Pos from, Pos to, bool icase, Pos& s, Pos& e) override;
	std::string group(int n) override;
	Pos topLine() override;
	void setTopLine(Pos line) override;
	Pos visibleLines() override;
	std::string encode(char32_t c) override;
	Keys decode(const std::string& s) override;
	std::string mapCase(const std::string& s, CaseMode m) override;
	std::string clipboard() override;
	void setClipboard(const std::string& s) override;
	void setCaretStyle(bool block) override;
	void setOvertype(bool on) override;
	void cancelPopups() override;

	UINT codePage();

private:
	SciFnDirect fn;
	Pos matchS = 0, matchE = 0;
	sptr_t ptr;
	sptr_t S(unsigned int m, uptr_t w = 0, sptr_t l = 0) { return fn(ptr, m, w, l); }
};
