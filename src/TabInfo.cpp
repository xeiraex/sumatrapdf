/* Copyright 2019 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/FileWatcher.h"
#include "utils/WinUtil.h"

#include "TreeModel.h"
#include "EngineBase.h"
#include "EngineManager.h"
#include "SettingsStructs.h"
#include "Controller.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "TabInfo.h"
#include "AppUtil.h"
#include "Selection.h"
#include "Translations.h"
#include "ParseBKM.h"

TabInfo::TabInfo(const WCHAR* filePath) {
    this->filePath.SetCopy(filePath);
}

TabInfo::~TabInfo() {
    FileWatcherUnsubscribe(watcher);
    if (AsChm()) {
        AsChm()->RemoveParentHwnd();
    }
    if (altBookmarks) {
        DeleteVecMembers(*altBookmarks);
        delete altBookmarks;
    }
    delete selectionOnPage;
    delete ctrl;
}

Kind TabInfo::GetEngineType() const {
    if (ctrl && ctrl->AsFixed()) {
        return ctrl->AsFixed()->GetEngine()->kind;
    }
    return nullptr;
}

const WCHAR* TabInfo::GetTabTitle() const {
    if (gGlobalPrefs->fullPathInTitle) {
        return filePath;
    }
    return path::GetBaseNameNoFree(filePath);
}

LinkSaver::LinkSaver(TabInfo* tab, HWND parentHwnd, const WCHAR* fileName) {
    this->tab = tab;
    this->parentHwnd = parentHwnd;
    this->fileName = fileName;
}

bool LinkSaver::SaveEmbedded(const unsigned char* data, size_t len) {
    if (!HasPermission(Perm_DiskAccess))
        return false;

    WCHAR dstFileName[MAX_PATH] = {0};
    if (fileName) {
        str::BufSet(dstFileName, dimof(dstFileName), fileName);
    }
    CrashIf(fileName && str::FindChar(fileName, '/'));

    // Prepare the file filters (use \1 instead of \0 so that the
    // double-zero terminated string isn't cut by the string handling
    // methods too early on)
    AutoFreeWstr fileFilter(str::Format(L"%s\1*.*\1", _TR("All files")));
    str::TransChars(fileFilter, L"\1", L"\0");

    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = this->parentHwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = fileFilter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    bool ok = GetSaveFileName(&ofn);
    if (!ok) {
        return false;
    }
    ok = file::WriteFile(dstFileName, {(char*)data, len});
    if (ok && tab && IsUntrustedFile(tab->filePath, gPluginURL)) {
        file::SetZoneIdentifier(dstFileName);
    }
    return ok;
}
