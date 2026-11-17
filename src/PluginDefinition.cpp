//this file is part of notepad++
//Copyright (C)2022 Don HO <don.h@free.fr>
//
//This program is free software; you can redistribute it and/or
//modify it under the terms of the GNU General Public License
//as published by the Free Software Foundation; either
//version 2 of the License, or (at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.
//
//You should have received a copy of the GNU General Public License
//along with this program; if not, write to the Free Software
//Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

#include "PluginDefinition.h"
#include "menuCmdID.h"
#include "SciEditor.h"
#include "Vim.h"

#include <imm.h>
#include <shlwapi.h>
#include <string>

//
// The plugin data that Notepad++ needs
//
FuncItem funcItem[nbFunc];

//
// The data of Notepad++ that you can use in your plugin commands
//
NppData nppData;

static HHOOK msgHook = NULL;
static bool vimEnabled = true;
static wchar_t highSurrogate = 0;

static HWND currentSci()
{
	int which = 0;
	::SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
	return which == 1 ? nppData._scintillaSecondHandle : nppData._scintillaMainHandle;
}

static HWND otherSci(HWND h)
{
	return h == nppData._scintillaMainHandle ? nppData._scintillaSecondHandle : nppData._scintillaMainHandle;
}

static std::wstring utf8ToWide(const std::string& s) { return toWide(s, CP_UTF8); }

static std::wstring iniPath()
{
	wchar_t dir[MAX_PATH] = {};
	::SendMessage(nppData._nppHandle, NPPM_GETPLUGINSCONFIGDIR, MAX_PATH, (LPARAM)dir);
	return std::wstring(dir) + L"\\viNotepad.ini";
}

struct NppHost : Host {
	void status(const std::string& s) override
	{
		std::wstring w = utf8ToWide(s);
		::SendMessage(nppData._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, (LPARAM)w.c_str());
	}

	bool ex(Editor&, const std::string& cmd, const std::string& arg, bool bang, std::string& err) override
	{
		HWND npp = nppData._nppHandle;
		HWND sci = currentSci();
		auto is = [&](std::initializer_list<const char*> names) {
			for (const char* n : names) if (cmd == n) return true;
			return false;
		};
		auto modified = [&]() { return ::SendMessage(sci, SCI_GETMODIFY, 0, 0) != 0; };
		auto save = [&]() { ::SendMessage(npp, NPPM_SAVECURRENTFILE, 0, 0); };
		auto post = [&](int id) { ::PostMessage(npp, WM_COMMAND, id, 0); };
		auto close = [&]() {
			if (bang) ::SendMessage(sci, SCI_SETSAVEPOINT, 0, 0);
			else if (modified()) { err = "E37: No write since last change (add ! to override)"; return; }
			post(IDM_FILE_CLOSE);
		};
		if (is({ "w", "write" })) {
			if (arg.empty()) save();
			else ::SendMessage(npp, NPPM_SAVECURRENTFILEAS, 1, (LPARAM)resolvePath(arg).c_str());
			return true;
		}
		if (is({ "wa", "wall" })) { ::SendMessage(npp, NPPM_SAVEALLFILES, 0, 0); return true; }
		if (is({ "q", "quit", "clo", "close", "bd", "bdelete", "bw", "bwipeout", "tabc", "tabclose" })) { close(); return true; }
		if (is({ "wq" })) { save(); post(IDM_FILE_CLOSE); return true; }
		if (is({ "x", "xit", "exi", "exit" })) { if (modified()) save(); post(IDM_FILE_CLOSE); return true; }
		if (is({ "qa", "qall", "quita", "quitall" })) { post(IDM_FILE_EXIT); return true; }
		if (is({ "wqa", "wqall", "xa", "xall" })) { ::SendMessage(npp, NPPM_SAVEALLFILES, 0, 0); post(IDM_FILE_EXIT); return true; }
		if (is({ "e", "edit", "tabe", "tabedit", "tabnew" })) {
			if (!arg.empty()) ::SendMessage(npp, NPPM_DOOPEN, 0, (LPARAM)resolvePath(arg).c_str());
			else if (cmd == "tabnew") post(IDM_FILE_NEW);
			else if (bang) post(IDM_FILE_RELOAD);
			return true;
		}
		if (is({ "ene", "enew", "new", "vne", "vnew" })) { post(IDM_FILE_NEW); return true; }
		if (is({ "bn", "bnext", "tabn", "tabnext" })) { post(IDM_VIEW_TAB_NEXT); return true; }
		if (is({ "bp", "bprevious", "bN", "bNext", "tabp", "tabprevious", "tabN", "tabNext" })) { post(IDM_VIEW_TAB_PREV); return true; }
		if (is({ "sp", "split", "vs", "vsplit" })) { post(IDM_VIEW_CLONE_TO_ANOTHER_VIEW); return true; }
		return false;
	}

	void switchView() override { ::PostMessage(nppData._nppHandle, WM_COMMAND, IDM_VIEW_SWITCHTO_OTHER_VIEW, 0); }

	void optionChanged() override;

	static std::wstring resolvePath(const std::string& arg)
	{
		std::wstring p = utf8ToWide(arg);
		if (!PathIsRelativeW(p.c_str())) return p;
		wchar_t dir[MAX_PATH] = {};
		::SendMessage(nppData._nppHandle, NPPM_GETCURRENTDIRECTORY, MAX_PATH, (LPARAM)dir);
		wchar_t out[MAX_PATH] = {};
		if (dir[0] && PathCombineW(out, dir, p.c_str())) return out;
		return p;
	}
};

static NppHost host;
static Vim vim(host);

static void saveSettings()
{
	std::wstring ini = iniPath();
	WritePrivateProfileStringW(L"viNotepad", L"enabled", vimEnabled ? L"1" : L"0", ini.c_str());
	WritePrivateProfileStringW(L"viNotepad", L"clipboard", vim.clipboardUnnamed ? L"1" : L"0", ini.c_str());
}

static void loadSettings()
{
	std::wstring ini = iniPath();
	vimEnabled = GetPrivateProfileIntW(L"viNotepad", L"enabled", 1, ini.c_str()) != 0;
	vim.clipboardUnnamed = GetPrivateProfileIntW(L"viNotepad", L"clipboard", 0, ini.c_str()) != 0;
}

void NppHost::optionChanged()
{
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[1]._cmdID, vim.clipboardUnnamed);
	saveSettings();
}

static void restoreStatus()
{
	int lang = 0;
	::SendMessage(nppData._nppHandle, NPPM_GETCURRENTLANGTYPE, 0, (LPARAM)&lang);
	wchar_t desc[256] = {};
	if (::SendMessage(nppData._nppHandle, NPPM_GETLANGUAGEDESC, lang, 0) < 255) ::SendMessage(nppData._nppHandle, NPPM_GETLANGUAGEDESC, lang, (LPARAM)desc);
	::SendMessage(nppData._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, (LPARAM)desc);
}

static void syncIme()
{
	static int state[2] = { -1, -1 };
	HWND views[2] = { nppData._scintillaMainHandle, nppData._scintillaSecondHandle };
	bool on = !vimEnabled || !vim.wantsChars();
	for (int i = 0; i < 2; i++) {
		if (!views[i] || state[i] == (int)on) continue;
		state[i] = (int)on;
		if (on) ImmAssociateContextEx(views[i], NULL, IACE_DEFAULT);
		else ImmAssociateContextEx(views[i], NULL, 0);
	}
}

static void feedKey(HWND h, char32_t k)
{
	SciEditor e(h);
	vim.key(e, k);
	syncIme();
}

static void imeResult(HWND h)
{
	HIMC imc = ImmGetContext(h);
	if (!imc) return;
	LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, NULL, 0);
	std::wstring w;
	if (bytes > 0) {
		w.resize((size_t)bytes / sizeof(wchar_t));
		ImmGetCompositionStringW(imc, GCS_RESULTSTR, &w[0], (DWORD)bytes);
	}
	ImmReleaseContext(h, imc);
	SciEditor e(h);
	for (size_t i = 0; i < w.size(); i++) {
		char32_t c = w[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
		vim.observe(e, c);
	}
}

static bool handleKeyDown(MSG* m)
{
	bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
	if (alt) return false;
	UINT vk = (UINT)m->wParam;
	char32_t k = 0;
	if (vim.insertLike() && !vim.wantsChars()) {
		if (vk == VK_ESCAPE) k = K_ESC;
		else if (ctrl && vk == VK_OEM_4) k = K_ESC;
		else if (ctrl && !shift && (vk == 'W' || vk == 'U' || vk == 'R')) k = ctrlKey((char)vk);
		if (!k) return false;
		feedKey(m->hwnd, k);
		return true;
	}
	switch (vk) {
	case VK_ESCAPE: k = K_ESC; break;
	case VK_RETURN: k = K_ENTER; break;
	case VK_BACK: k = K_BS; break;
	case VK_TAB: if (ctrl) return false; k = K_TAB; break;
	}
	if (!k && !ctrl && !shift) {
		switch (vk) {
		case VK_UP: k = K_UP; break;
		case VK_DOWN: k = K_DOWN; break;
		case VK_LEFT: k = K_LEFT; break;
		case VK_RIGHT: k = K_RIGHT; break;
		case VK_HOME: k = K_HOME; break;
		case VK_END: k = K_END; break;
		case VK_PRIOR: k = K_PGUP; break;
		case VK_NEXT: k = K_PGDN; break;
		case VK_DELETE: k = K_DEL; break;
		case VK_INSERT: k = K_INS; break;
		}
	}
	if (!k && ctrl && !shift) {
		if (vk >= 'A' && vk <= 'Z' && strchr("ABDEFRUVWXY", (int)vk)) k = ctrlKey((char)vk);
		else if (vk == VK_OEM_4) k = K_ESC;
	}
	if (!k) return false;
	feedKey(m->hwnd, k);
	return true;
}

static bool handleChar(MSG* m)
{
	wchar_t ch = (wchar_t)m->wParam;
	char32_t c = ch;
	if (ch >= 0xD800 && ch < 0xDC00) { highSurrogate = ch; return vim.wantsChars(); }
	if (ch >= 0xDC00 && ch < 0xE000) {
		if (!highSurrogate) return vim.wantsChars();
		c = 0x10000 + ((char32_t)(highSurrogate - 0xD800) << 10) + (ch - 0xDC00);
		highSurrogate = 0;
	}
	if (!vim.wantsChars()) {
		SciEditor e(m->hwnd);
		vim.observe(e, c);
		return false;
	}
	if (c < 32) return true;
	feedKey(m->hwnd, c);
	return true;
}

static LRESULT CALLBACK getMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && wParam == PM_REMOVE && vimEnabled) {
		MSG* m = (MSG*)lParam;
		if (m->hwnd && (m->hwnd == nppData._scintillaMainHandle || m->hwnd == nppData._scintillaSecondHandle)) {
			bool eat = false;
			if (m->message == WM_KEYDOWN) eat = handleKeyDown(m);
			else if (m->message == WM_CHAR) eat = handleChar(m);
			else if (m->message == WM_IME_COMPOSITION && (m->lParam & GCS_RESULTSTR) && vim.insertLike() && !vim.wantsChars()) imeResult(m->hwnd);
			if (eat) {
				m->message = WM_NULL;
				m->wParam = 0;
				m->lParam = 0;
			}
		}
	}
	return CallNextHookEx(msgHook, code, wParam, lParam);
}

//
// Initialize your plugin data here
// It will be called while plugin loading   
void pluginInit(HANDLE /*hModule*/)
{
}

//
// Here you can do the clean up, save the parameters (if any) for the next session
//
void pluginCleanUp()
{
}

//
// Initialization of your plugin commands
// You should fill your plugins commands here
void commandMenuInit()
{

    //--------------------------------------------//
    //-- STEP 3. CUSTOMIZE YOUR PLUGIN COMMANDS --//
    //--------------------------------------------//
    // with function :
    // setCommand(int index,                      // zero based number to indicate the order of command
    //            TCHAR *commandName,             // the command name that you want to see in plugin menu
    //            PFUNCPLUGINCMD functionPointer, // the symbol of function (function pointer) associated with this command. The body should be defined below. See Step 4.
    //            ShortcutKey *shortcut,          // optional. Define a shortcut to trigger this command
    //            bool check0nInit                // optional. Make this menu item be checked visually
    //            );
    loadSettings();
    setCommand(0, TEXT("Vim Mode"), toggleVim, NULL, vimEnabled);
    setCommand(1, TEXT("Yank to System Clipboard"), toggleClipboard, NULL, vim.clipboardUnnamed);
    setCommand(2, TEXT("About viNotepad"), about, NULL, false);
}

//
// Here you can do the clean up (especially for the shortcut)
//
void commandMenuCleanUp()
{
	// Don't forget to deallocate your shortcut here
	if (msgHook) {
		UnhookWindowsHookEx(msgHook);
		msgHook = NULL;
	}
}


//
// This function help you to initialize your plugin commands
//
bool setCommand(size_t index, TCHAR *cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey *sk, bool check0nInit) 
{
    if (index >= nbFunc)
        return false;

    if (!pFunc)
        return false;

    lstrcpy(funcItem[index]._itemName, cmdName);
    funcItem[index]._pFunc = pFunc;
    funcItem[index]._init2Check = check0nInit;
    funcItem[index]._pShKey = sk;

    return true;
}

//----------------------------------------------//
//-- STEP 4. DEFINE YOUR ASSOCIATED FUNCTIONS --//
//----------------------------------------------//
void toggleVim()
{
	vimEnabled = !vimEnabled;
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[0]._cmdID, vimEnabled);
	HWND cur = currentSci();
	SciEditor e(cur);
	if (vimEnabled) {
		SciEditor(otherSci(cur)).setCaretStyle(true);
		vim.enter(e);
	} else {
		vim.leave(e);
		SciEditor(otherSci(cur)).setCaretStyle(false);
		restoreStatus();
	}
	syncIme();
	saveSettings();
}

void toggleClipboard()
{
	vim.clipboardUnnamed = !vim.clipboardUnnamed;
	host.optionChanged();
}

void about()
{
	::MessageBox(nppData._nppHandle,
		TEXT("viNotepad: vim emulation for Notepad++\r\n\r\n")
		TEXT("Modes: normal, insert, replace, visual (v, V, Ctrl+V), command line.\r\n")
		TEXT("Motions: h j k l w b e W B E ge 0 ^ $ g_ gg G { } % f F t T ; , H M L n N * # marks.\r\n")
		TEXT("Operators: d c y > < g~ gu gU with counts, registers and text objects (iw aw ip ap i\" a\" i( a( i{ a{ i[ a[ i< a<).\r\n")
		TEXT("Other: x X D C s S Y p P J gJ r R ~ u Ctrl+R . q @ Ctrl+A Ctrl+X Ctrl+D Ctrl+U Ctrl+F Ctrl+B Ctrl+E Ctrl+Y zz zt zb.\r\n")
		TEXT("Ex: :w :q :q! :wq :x :wa :qa :e file :bn :bp :vs :N :s :%s :d :y :j :> :< :normal :noh :reg :marks :set clipboard=unnamed.\r\n\r\n")
		TEXT("The current mode is shown in the status bar."),
		TEXT("About viNotepad"), MB_OK);
}

void vimReady()
{
	sciInsertCaretStyle = (int)::SendMessage(nppData._scintillaMainHandle, SCI_GETCARETSTYLE, 0, 0);
	if (sciInsertCaretStyle == CARETSTYLE_BLOCK) sciInsertCaretStyle = CARETSTYLE_LINE;
	msgHook = SetWindowsHookEx(WH_GETMESSAGE, getMsgProc, NULL, GetCurrentThreadId());
	if (!vimEnabled) return;
	HWND cur = currentSci();
	SciEditor(otherSci(cur)).setCaretStyle(true);
	SciEditor e(cur);
	vim.enter(e);
	syncIme();
}

void vimBufferActivated()
{
	if (!vimEnabled || !msgHook) return;
	SciEditor e(currentSci());
	vim.bufferActivated(e);
	syncIme();
}

void vimTextChanged(HWND hwnd, intptr_t pos, intptr_t len, bool inserted)
{
	if (!msgHook) return;
	if (hwnd != nppData._scintillaMainHandle && hwnd != nppData._scintillaSecondHandle) return;
	if (hwnd == nppData._scintillaSecondHandle && ::SendMessage(hwnd, SCI_GETDOCPOINTER, 0, 0) == ::SendMessage(nppData._scintillaMainHandle, SCI_GETDOCPOINTER, 0, 0)) return;
	SciEditor e(hwnd);
	vim.textChanged(e, pos, len, inserted);
}

void vimSelectionChanged(HWND hwnd)
{
	if (!vimEnabled || !msgHook || hwnd != currentSci()) return;
	SciEditor e(hwnd);
	vim.selectionChanged(e);
	syncIme();
}
