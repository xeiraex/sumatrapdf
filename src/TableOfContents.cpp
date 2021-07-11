/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/Log.h"
#include "utils/BitManip.h"
#include "utils/FileUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/WinGui.h"
#include "wingui/Layout.h"
#include "wingui/Window.h"
#include "wingui/LabelWithCloseWnd.h"
#include "wingui/SplitterWnd.h"
#include "wingui/TreeModel.h"
#include "wingui/TreeCtrl.h"
#include "wingui/DropDownCtrl.h"

#include "utils/GdiPlusUtil.h"

#include "Annotation.h"
#include "EngineBase.h"
#include "EnginePdf.h"
#include "EngineCreate.h"

#include "SumatraConfig.h"
#include "DisplayMode.h"
#include "SettingsStructs.h"
#include "Controller.h"
#include "GlobalPrefs.h"
#include "AppColors.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "DisplayModel.h"
#include "Favorites.h"
#include "TabInfo.h"
#include "resource.h"
#include "Commands.h"
#include "AppTools.h"
#include "TableOfContents.h"
#include "Translations.h"
#include "Tabs.h"
#include "Menu.h"

/* Define if you want page numbers to be displayed in the ToC sidebar */
// #define DISPLAY_TOC_PAGE_NUMBERS

#ifdef DISPLAY_TOC_PAGE_NUMBERS
#define WM_APP_REPAINT_TOC (WM_APP + 1)
#endif

// set tooltip for this item but only if the text isn't fully shown
// TODO: I might have lost something in translation
static void TocCustomizeTooltip(TreeItmGetTooltipEvent* ev) {
    auto* w = ev->treeCtrl;
    auto* ti = ev->treeItem;
    auto* nm = ev->info;
    TocItem* tocItem = (TocItem*)ti;
    PageDestination* link = tocItem->GetPageDestination();
    if (!link) {
        return;
    }
    WCHAR* path = link->GetValue();
    if (!path) {
        path = tocItem->title;
    }
    if (!path) {
        return;
    }
    auto k = link->Kind();
    // TODO: TocItem from Chm contain other types
    // we probably shouldn't set TocItem::dest there
    if (k == kindDestinationScrollTo) {
        return;
    }
    if (k == kindDestinationNone) {
        return;
    }

    CrashIf(k != kindDestinationLaunchURL && k != kindDestinationLaunchFile && k != kindDestinationLaunchEmbedded);

    str::WStr infotip;

    // Display the item's full label, if it's overlong
    RECT rcLine, rcLabel;
    w->GetItemRect(ev->treeItem, false, rcLine);
    w->GetItemRect(ev->treeItem, true, rcLabel);

    if (rcLine.right + 2 < rcLabel.right) {
        str::WStr currInfoTip = ti->Text();
        infotip.Append(currInfoTip.Get());
        infotip.Append(L"\r\n");
    }

    if (kindDestinationLaunchEmbedded == k) {
        AutoFreeWstr tmp = str::Format(_TR("Attachment: %s"), path);
        infotip.Append(tmp.Get());
    } else {
        infotip.Append(path);
    }

    str::BufSet(nm->pszText, nm->cchTextMax, infotip.Get());
    ev->didHandle = true;
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void RelayoutTocItem(LPNMTVCUSTOMDRAW ntvcd) {
    // code inspired by http://www.codeguru.com/cpp/controls/treeview/multiview/article.php/c3985/
    LPNMCUSTOMDRAW ncd = &ntvcd->nmcd;
    HWND hTV = ncd->hdr.hwndFrom;
    HTREEITEM hItem = (HTREEITEM)ncd->dwItemSpec;
    RECT rcItem;
    if (0 == ncd->rc.right - ncd->rc.left || 0 == ncd->rc.bottom - ncd->rc.top)
        return;
    if (!TreeView_GetItemRect(hTV, hItem, &rcItem, TRUE))
        return;
    if (rcItem.right > ncd->rc.right)
        rcItem.right = ncd->rc.right;

    // Clear the label
    RECT rcFullWidth = rcItem;
    rcFullWidth.right = ncd->rc.right;
    FillRect(ncd->hdc, &rcFullWidth, GetSysColorBrush(COLOR_WINDOW));

    // Get the label's text
    WCHAR szText[MAX_PATH];
    TVITEM item;
    item.hItem = hItem;
    item.mask = TVIF_TEXT | TVIF_PARAM;
    item.pszText = szText;
    item.cchTextMax = MAX_PATH;
    TreeView_GetItem(hTV, &item);

    // Draw the page number right-aligned (if there is one)
    WindowInfo* win = FindWindowInfoByHwnd(hTV);
    TocItem* tocItem = (TocItem*)item.lParam;
    AutoFreeWstr label;
    if (tocItem->pageNo && win && win->IsDocLoaded()) {
        label.Set(win->ctrl->GetPageLabel(tocItem->pageNo));
        label.Set(str::Join(L"  ", label));
    }
    if (label && str::EndsWith(item.pszText, label)) {
        RECT rcPageNo = rcFullWidth;
        InflateRect(&rcPageNo, -2, -1);

        SIZE txtSize;
        GetTextExtentPoint32(ncd->hdc, label, str::Len(label), &txtSize);
        rcPageNo.left = rcPageNo.right - txtSize.cx;

        SetTextColor(ncd->hdc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(ncd->hdc, GetSysColor(COLOR_WINDOW));
        DrawText(ncd->hdc, label, -1, &rcPageNo, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        // Reduce the size of the label and cut off the page number
        rcItem.right = std::max(rcItem.right - txtSize.cx, 0);
        szText[str::Len(szText) - str::Len(label)] = '\0';
    }

    SetTextColor(ncd->hdc, ntvcd->clrText);
    SetBkColor(ncd->hdc, ntvcd->clrTextBk);

    // Draw the focus rectangle (including proper background color)
    HBRUSH brushBg = CreateSolidBrush(ntvcd->clrTextBk);
    FillRect(ncd->hdc, &rcItem, brushBg);
    DeleteObject(brushBg);
    if ((ncd->uItemState & CDIS_FOCUS))
        DrawFocusRect(ncd->hdc, &rcItem);

    InflateRect(&rcItem, -2, -1);
    DrawText(ncd->hdc, szText, -1, &rcItem, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_WORD_ELLIPSIS);
}
#endif

static void GoToTocLinkTask(TocItem* tocItem, TabInfo* tab, Controller* ctrl) {
    WindowInfo* win = tab->win;
    // tocItem is invalid if the Controller has been replaced
    if (!WindowInfoStillValid(win) || win->currentTab != tab || tab->ctrl != ctrl) {
        return;
    }

    // make sure that the tree item that the user selected
    // isn't unselected in UpdateTocSelection right again
    win->tocKeepSelection = true;
    int pageNo = tocItem->pageNo;
    PageDestination* dest = tocItem->GetPageDestination();
    if (dest) {
        win->linkHandler->GotoLink(dest);
    } else if (pageNo) {
        ctrl->GoToPage(pageNo, true);
    }
    win->tocKeepSelection = false;
}

static bool IsScrollToLink(PageDestination* link) {
    if (!link) {
        return false;
    }
    auto kind = link->Kind();
    return kind == kindDestinationScrollTo;
}

static void GoToTocTreeItem(WindowInfo* win, TreeItem* ti, bool allowExternal) {
    if (!ti) {
        return;
    }
    TocItem* tocItem = (TocItem*)ti;
    bool validPage = (tocItem->pageNo > 0);
    bool isScroll = IsScrollToLink(tocItem->GetPageDestination());
    if (validPage || (allowExternal || isScroll)) {
        // delay changing the page until the tree messages have been handled
        TabInfo* tab = win->currentTab;
        Controller* ctrl = win->ctrl;
        uitask::Post([=] { GoToTocLinkTask(tocItem, tab, ctrl); });
    }
}

void ClearTocBox(WindowInfo* win) {
    if (!win->tocLoaded) {
        return;
    }

    win->tocTreeCtrl->Clear();

    win->currPageNo = 0;
    win->tocLoaded = false;
}

void ToggleTocBox(WindowInfo* win) {
    if (!win->IsDocLoaded()) {
        return;
    }
    if (win->tocVisible) {
        SetSidebarVisibility(win, false, gGlobalPrefs->showFavorites);
        return;
    }
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    if (win->tocVisible) {
        win->tocTreeCtrl->SetFocus();
    }
}

// find the closest item in tree view to a given page number
static TreeItem* TreeItemForPageNo(TreeCtrl* treeCtrl, int pageNo) {
    TocItem* bestMatch = nullptr;
    int bestMatchPageNo = 0;

    TreeModel* tm = treeCtrl->treeModel;
    if (!tm) {
        return nullptr;
    }
    int nItems = 0;
    VisitTreeModelItems(tm, [&](TreeItem* ti) {
        auto* docItem = (TocItem*)ti;
        if (!docItem) {
            return true;
        }
        if (!bestMatch) {
            // if nothing else matches, match the root node
            bestMatch = docItem;
        }
        ++nItems;
        int page = docItem->pageNo;
        if ((page <= pageNo) && (page >= bestMatchPageNo) && (page >= 1)) {
            bestMatch = docItem;
            bestMatchPageNo = page;
            if (pageNo == bestMatchPageNo) {
                // we can stop earlier if we found the exact match
                return false;
            }
        }
        return true;
    });
    // if there's only one item, we want to unselect it so that it can
    // be selected by the user
    if (nItems < 2) {
        return nullptr;
    }
    return bestMatch;
}

// TODO: I can't use TreeItem->IsExpanded() because it's not in sync with
// the changes user makes to TreeCtrl
static TreeItem* FindVisibleParentTreeItem(TreeCtrl* treeCtrl, TreeItem* ti) {
    if (!ti) {
        return nullptr;
    }
    while (true) {
        auto parent = ti->Parent();
        if (parent == nullptr) {
            // ti is a root node
            return ti;
        }
        if (treeCtrl->IsExpanded(parent)) {
            return ti;
        }
        ti = parent;
    }
    return nullptr;
}

void UpdateTocSelection(WindowInfo* win, int currPageNo) {
    if (!win->tocLoaded || !win->tocVisible || win->tocKeepSelection) {
        return;
    }

    auto treeCtrl = win->tocTreeCtrl;
    TreeItem* item = TreeItemForPageNo(treeCtrl, currPageNo);
    // only select the items that are visible i.e. are top nodes or
    // children of expanded node
    TreeItem* toSelect = FindVisibleParentTreeItem(treeCtrl, item);
    treeCtrl->SelectItem(toSelect);
}

static void UpdateDocTocExpansionStateRecur(TreeCtrl* treeCtrl, Vec<int>& tocState, TocItem* tocItem) {
    while (tocItem) {
        // items without children cannot be toggled
        if (tocItem->child) {
            // we have to query the state of the tree view item because
            // isOpenToggled is not kept in sync
            // TODO: keep toggle state on TocItem in sync
            // by subscribing to the right notifications
            bool isExpanded = treeCtrl->IsExpanded(tocItem);
            bool wasToggled = isExpanded != tocItem->isOpenDefault;
            if (wasToggled) {
                tocState.Append(tocItem->id);
            }
            UpdateDocTocExpansionStateRecur(treeCtrl, tocState, tocItem->child);
        }
        tocItem = tocItem->next;
    }
}

void UpdateTocExpansionState(Vec<int>& tocState, TreeCtrl* treeCtrl, TocTree* docTree) {
    if (treeCtrl->treeModel != docTree) {
        // CrashMe();
        return;
    }
    tocState.Reset();
    TocItem* tocItem = docTree->root;
    UpdateDocTocExpansionStateRecur(treeCtrl, tocState, tocItem);
}

static bool inRange(WCHAR c, WCHAR low, WCHAR hi) {
    return (low <= c) && (c <= hi);
}

// copied from mupdf/fitz/dev_text.c
// clang-format off
static bool isLeftToRightChar(WCHAR c) {
    return (
        inRange(c, 0x0041, 0x005A) ||
        inRange(c, 0x0061, 0x007A) ||
        inRange(c, 0xFB00, 0xFB06)
    );
}

static bool isRightToLeftChar(WCHAR c) {
    return (
        inRange(c, 0x0590, 0x05FF) ||
        inRange(c, 0x0600, 0x06FF) ||
        inRange(c, 0x0750, 0x077F) ||
        inRange(c, 0xFB50, 0xFDFF) ||
        inRange(c, 0xFE70, 0xFEFE)
    );
}
// clang-format off

static void GetLeftRightCounts(TocItem* node, int& l2r, int& r2l) {
next:
    if (!node) {
        return;
    }
    // short-circuit because this could overflow the stack due to recursion
    // (happened in doc from https://github.com/sumatrapdfreader/sumatrapdf/issues/1795)
    if (l2r + r2l > 1024) {
        return;
    }
    if (node->title) {
        for (const WCHAR* c = node->title; *c; c++) {
            if (isLeftToRightChar(*c)) {
                l2r++;
            } else if (isRightToLeftChar(*c)) {
                r2l++;
            }
        }
    }
    GetLeftRightCounts(node->child, l2r, r2l);
    // could be: GetLeftRightCounts(node->next, l2r, r2l);
    // but faster if not recursive
    node = node->next;
    goto next;
}

static void SetInitialExpandState(TocItem* item, Vec<int>& tocState) {
    while (item) {
        if (tocState.Contains(item->id)) {
            item->isOpenToggled = true;
        }
        SetInitialExpandState(item->child, tocState);
        item = item->next;
    }
}

static void AddFavoriteFromToc(WindowInfo* win, TocItem* dti) {
    int pageNo = 0;
    if (!dti) {
        return;
    }
    if (dti->dest) {
        pageNo = dti->dest->GetPageNo();
    }
    AutoFreeWstr name = str::Dup(dti->title);
    AutoFreeWstr pageLabel = win->ctrl->GetPageLabel(pageNo);
    AddFavoriteWithLabelAndName(win, pageNo, pageLabel.Get(), name);
}

static void OpenEmbeddedFile(TabInfo* tab, PageDestination* dest) {
    CrashIf(!tab || !dest);
    if (!tab || !dest) {
        return;
    }
    auto win = tab->win;
    WCHAR* path = dest->GetValue();
    if (!str::StartsWith(path, tab->filePath.Get())) {
        return;
    }
    WindowInfo* newWin = FindWindowInfoByFile(path, true);
    if (!newWin) {
        LoadArgs args(path, win);
        newWin = LoadDocument(args);
    }
    if (newWin) {
        newWin->Focus();
    }
}

static void SaveEmbeddedFile(TabInfo* tab, PageDestination* dest) {
    CrashIf(!tab || !dest);
    if (!tab || !dest) {
        return;
    }
    auto filePath = dest->GetValue();
    auto data = LoadEmbeddedPDFFile(filePath);
    AutoFreeWstr dir = path::GetDir(filePath);
    auto fileName = dest->GetName();
    AutoFreeWstr dstPath = path::Join(dir, fileName);
#if 0 // TODO: why did I have it here?
    int streamNo = -1;
    AutoFreeWstr fileSystemPath = ParseEmbeddedStreamNumber(filePath, &streamNo);
#endif
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data.data());
}

static void TocContextMenu(ContextMenuEvent* ev) {
    WindowInfo* win = FindWindowInfoByHwnd(ev->w->hwnd);
    const WCHAR* filePath = win->ctrl->FilePath();

    POINT pt{};
    TreeItem* ti = GetOrSelectTreeItemAtPos(ev, pt);
    if (!ti) {
        pt = {ev->mouseGlobal.x, ev->mouseGlobal.y};
    }
    int pageNo = 0;
    TocItem* dti = (TocItem*)ti;
    if (dti && dti->dest) {
        pageNo = dti->dest->GetPageNo();
    }

    TabInfo* tab = win->currentTab;
    HMENU popup = BuildMenuFromMenuDef(menuDefContextToc, CreatePopupMenu(), 0);

    bool isEmbeddedFile = false;
    PageDestination* dest = nullptr;
    WCHAR* path = nullptr;
    if (dti && dti->dest) {
        dest = dti->dest;
        path = dest->GetValue();
        isEmbeddedFile = (path != nullptr) && (dest->kind == kindDestinationLaunchEmbedded);
    }
    if (isEmbeddedFile) {
        auto embeddedName = dest->GetName();
        const WCHAR* ext = path::GetExtNoFree(embeddedName);
        bool canOpenEmbedded = str::EqI(ext, L".pdf");
        if (!canOpenEmbedded) {
            win::menu::Remove(popup, CmdOpenEmbeddedPDF);
        }
    } else {
        win::menu::Remove(popup, CmdSeparatorEmbed);
        win::menu::Remove(popup, CmdSaveEmbeddedFile);
        win::menu::Remove(popup, CmdOpenEmbeddedPDF);
    }

    if (pageNo > 0) {
        AutoFreeWstr pageLabel = win->ctrl->GetPageLabel(pageNo);
        bool isBookmarked = gFavorites.IsPageInFavorites(filePath, pageNo);
        if (isBookmarked) {
            win::menu::Remove(popup, CmdFavoriteAdd);

            // %s and not %d because re-using translation from RebuildFavMenu()
            auto tr = _TR("Remove page %s from favorites");
            AutoFreeWstr s = str::Format(tr, pageLabel.Get());
            win::menu::SetText(popup, CmdFavoriteDel, s);
        } else {
            win::menu::Remove(popup, CmdFavoriteDel);

            // %s and not %d because re-using translation from RebuildFavMenu()
            auto tr = _TR("Add page %s to favorites");
            AutoFreeWstr s = str::Format(tr, pageLabel.Get());
            win::menu::SetText(popup, CmdFavoriteAdd, s);
        }
    } else {
        win::menu::Remove(popup, CmdFavoriteAdd);
        win::menu::Remove(popup, CmdFavoriteDel);
    }

    MarkMenuOwnerDraw(popup);
    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmd = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);
    switch (cmd) {
        case CmdExpandAll:
            win->tocTreeCtrl->ExpandAll();
            break;
        case CmdCollapseAll:
            win->tocTreeCtrl->CollapseAll();
            break;
        case CmdFavoriteAdd:
            AddFavoriteFromToc(win, dti);
            break;
        case CmdFavoriteDel:
            DelFavorite(filePath, pageNo);
            break;
        case CmdSaveEmbeddedFile:
            SaveEmbeddedFile(tab, dest);
            break;
        case CmdOpenEmbeddedPDF:
            OpenEmbeddedFile(tab, dest);
            break;
    }
}

static bool ShouldCustomDraw(WindowInfo* win) {
    // we only want custom drawing for pdf and pdf multi engines
    // as they are the only ones supporting custom colors and fonts
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }
    Kind kind = dm->GetEngineType();
    if (kind == kindEnginePdf || kind == kindEngineMulti) {
        return true;
    }
    return false;
}

void OnTocCustomDraw(TreeItemCustomDrawEvent*);

void LoadTocTree(WindowInfo* win) {
    TabInfo* tab = win->currentTab;
    CrashIf(!tab);

    if (win->tocLoaded) {
        return;
    }

    win->tocLoaded = true;

    auto* tocTree = tab->ctrl->GetToc();
    if (!tocTree || !tocTree->root) {
        return;
    }

    tab->currToc = tocTree;

    // consider a ToC tree right-to-left if a more than half of the
    // alphabetic characters are in a right-to-left script
    int l2r = 0, r2l = 0;
    GetLeftRightCounts(tocTree->root, l2r, r2l);
    bool isRTL = r2l > l2r;

    TreeCtrl* treeCtrl = win->tocTreeCtrl;
    HWND hwnd = treeCtrl->hwnd;
    SetRtl(hwnd, isRTL);

    UpdateTreeCtrlColors(win);
    SetInitialExpandState(tocTree->root, tab->tocState);
    tocTree->root->OpenSingleNode();

    treeCtrl->SetTreeModel(tocTree);

    treeCtrl->onTreeItemCustomDraw = nullptr;
    if (ShouldCustomDraw(win)) {
        treeCtrl->onTreeItemCustomDraw = OnTocCustomDraw;
    }
    LayoutTreeContainer(win->tocLabelWithClose, win->tocTreeCtrl->hwnd);
    // uint fl = RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN;
    // RedrawWindow(hwnd, nullptr, nullptr, fl);
}

// TODO: use https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getobject?redirectedfrom=MSDN
// to get LOGFONT from existing font and then create a derived font
static void UpdateFont(HDC hdc, int fontFlags) {
    // TODO: this is a bit hacky, in that we use default font
    // and not the font from TreeCtrl. But in this case they are the same
    bool italic = bit::IsSet(fontFlags, fontBitItalic);
    bool bold = bit::IsSet(fontFlags, fontBitBold);
    HFONT hfont = GetDefaultGuiFont(bold, italic);
    SelectObject(hdc, hfont);
}

// https://docs.microsoft.com/en-us/windows/win32/controls/about-custom-draw
// https://docs.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-nmtvcustomdraw
void OnTocCustomDraw(TreeItemCustomDrawEvent* ev) {
#if defined(DISPLAY_TOC_PAGE_NUMBERS)
    if (win->AsEbook())
        return CDRF_DODEFAULT;
    switch (((LPNMCUSTOMDRAW)pnmtv)->dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_DODEFAULT | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT:
            RelayoutTocItem((LPNMTVCUSTOMDRAW)pnmtv);
            // fall through
        default:
            return CDRF_DODEFAULT;
    }
    break;
#endif

    ev->result = CDRF_DODEFAULT;
    ev->didHandle = true;

    NMTVCUSTOMDRAW* tvcd = ev->nm;
    NMCUSTOMDRAW* cd = &(tvcd->nmcd);
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        // ask to be notified about each item
        ev->result = CDRF_NOTIFYITEMDRAW;
        return;
    }

    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        // called before drawing each item
        TocItem* tocItem = (TocItem*)ev->treeItem;
        if (!tocItem) {
            return;
        }
        if (tocItem->color != ColorUnset) {
            tvcd->clrText = tocItem->color;
        }
        if (tocItem->fontFlags != 0) {
            UpdateFont(cd->hdc, tocItem->fontFlags);
            ev->result = CDRF_NEWFONT;
            return;
        }
        return;
    }
    return;
}

static void TocTreeClick(TreeClickEvent* ev) {
    ev->didHandle = true;
    if (!ev->treeItem) {
        return;
    }
    WindowInfo* win = FindWindowInfoByHwnd(ev->w->hwnd);
    CrashIf(!win);
    bool allowExternal = false;
    GoToTocTreeItem(win, ev->treeItem, allowExternal);
}

static void TocTreeSelectionChanged(TreeSelectionChangedEvent* ev) {
    WindowInfo* win = FindWindowInfoByHwnd(ev->w->hwnd);
    CrashIf(!win);

    // When the focus is set to the toc window the first item in the treeview is automatically
    // selected and a TVN_SELCHANGEDW notification message is sent with the special code pnmtv->action ==
    // 0x00001000. We have to ignore this message to prevent the current page to be changed.
    // The case pnmtv->action==TVC_UNKNOWN is ignored because
    // it corresponds to a notification sent by
    // the function TreeView_DeleteAllItems after deletion of the item.
    bool shouldHandle = ev->byKeyboard || ev->byMouse;
    if (!shouldHandle) {
        return;
    }
    bool allowExternal = ev->byMouse;
    GoToTocTreeItem(win, ev->selectedItem, allowExternal);
    ev->didHandle = true;
}

// also used in Favorites.cpp
void TocTreeKeyDown(TreeKeyDownEvent* ev) {
    // TODO: trying to fix https://github.com/sumatrapdfreader/sumatrapdf/issues/1841
    // doesn't work i.e. page up / page down seems to be processed anyway by TreeCtrl
#if 0
    if ((ev->keyCode == VK_PRIOR) || (ev->keyCode == VK_NEXT)) {
        // up/down in tree is not very useful, so instead
        // send it to frame so that it scrolls document instead
        WindowInfo* win = FindWindowInfoByHwnd(ev->hwnd);
        // this is sent as WM_NOTIFY to TreeCtrl but for frame it's WM_KEYDOWN
        // alternatively, we could call FrameOnKeydown(ev->wp, ev->lp, false);
        SendMessageW(win->hwndFrame, WM_KEYDOWN, ev->wp, ev->lp);
        ev->didHandle = true;
        ev->result = 1;
        return;
    }
#endif
    if (ev->keyCode != VK_TAB) {
        return;
    }
    ev->didHandle = true;
    ev->result = 1;

    WindowInfo* win = FindWindowInfoByHwnd(ev->hwnd);
    if (win->tabsVisible && IsCtrlPressed()) {
        TabsOnCtrlTab(win, IsShiftPressed());
        return;
    }
    AdvanceFocus(win);
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void TocTreeMsgFilter([[maybe_unused]] WndEvent* ev) {
    switch (msg) {
        case WM_SIZE:
        case WM_HSCROLL:
            // Repaint the ToC so that RelayoutTocItem is called for all items
            PostMessageW(hwnd, WM_APP_REPAINT_TOC, 0, 0);
            break;
        case WM_APP_REPAINT_TOC:
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
            break;
    }
}
#endif

// Position label with close button and tree window within their parent.
// Used for toc and favorites.
void LayoutTreeContainer(LabelWithCloseWnd* l, HWND hwndTree) {
    HWND hwndContainer = GetParent(hwndTree);
    Size labelSize = l->GetIdealSize();
    Rect rc = WindowRect (hwndContainer);
    int dy = rc.dy;
    int y = 0;
    MoveWindow(l->hwnd, y, 0, rc.dx, labelSize.dy, TRUE);
    dy -= labelSize.dy;
    y += labelSize.dy;
    MoveWindow(hwndTree, 0, y, rc.dx, dy, TRUE);
}

static LRESULT CALLBACK WndProcTocBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId, DWORD_PTR data) {
    // this is a parent of TreeCtrl and DropDownCtrl
    // TODO: TreeCtrl and DropDownCtrl should be children of frame

    LRESULT res = 0;
    if (HandleRegisteredMessages(hwnd, msg, wp, lp, res)) {
        return res;
    }

    WindowInfo* winFromData = (WindowInfo*)(data);
    WindowInfo* win = FindWindowInfoByHwnd(hwnd);
    if (!win) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    CrashIf(subclassId != win->tocBoxSubclassId);
    CrashIf(win != winFromData);

    switch (msg) {
        case WM_SIZE:
            LayoutTreeContainer(win->tocLabelWithClose, win->tocTreeCtrl->hwnd);
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_TOC_LABEL_WITH_CLOSE) {
                ToggleTocBox(win);
            }
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SubclassToc(WindowInfo* win) {
    HWND hwndTocBox = win->hwndTocBox;

    if (win->tocBoxSubclassId == 0) {
        win->tocBoxSubclassId = NextSubclassId();
        BOOL ok = SetWindowSubclass(hwndTocBox, WndProcTocBox, win->tocBoxSubclassId, (DWORD_PTR)win);
        CrashIf(!ok);
    }
}

void UnsubclassToc(WindowInfo* win) {
    if (win->tocBoxSubclassId != 0) {
        RemoveWindowSubclass(win->hwndTocBox, WndProcTocBox, win->tocBoxSubclassId);
        win->tocBoxSubclassId = 0;
    }
}

void TocTreeMouseWheelHandler(MouseWheelEvent* ev) {
    WindowInfo* win = FindWindowInfoByHwnd(ev->hwnd);
    CrashIf(!win);
    if (!win) {
        return;
    }
    // scroll the canvas if the cursor isn't over the ToC tree
    if (!IsCursorOverWindow(ev->hwnd)) {
        ev->didHandle = true;
        ev->result = SendMessageW(win->hwndCanvas, ev->msg, ev->wp, ev->lp);
    }
}

void TocTreeCharHandler(CharEvent* ev) {
    WindowInfo* win = FindWindowInfoByHwnd(ev->hwnd);
    CrashIf(!win);
    if (!win) {
        return;
    }
    if (VK_ESCAPE != ev->keyCode) {
        return;
    }
    if (!gGlobalPrefs->escToExit) {
        return;
    }
    if (!MayCloseWindow(win)) {
        return;
    }

    CloseWindow(win, true, false);
    ev->didHandle = true;
}

extern  HFONT GetTreeFont();

void CreateToc(WindowInfo* win) {
    HMODULE hmod = GetModuleHandle(nullptr);
    int dx = gGlobalPrefs->sidebarDx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN;
    HWND parent = win->hwndFrame;
    win->hwndTocBox = CreateWindowExW(0, WC_STATIC, L"", style, 0, 0, dx, 0, parent, 0, hmod, nullptr);

    auto* l = new LabelWithCloseWnd();
    l->Create(win->hwndTocBox, IDC_TOC_LABEL_WITH_CLOSE);
    win->tocLabelWithClose = l;
    l->SetPaddingXY(2, 2);
    // TODO: use the same font size as in GetTreeFont()?
    l->SetFont(GetDefaultGuiFont(true, false));
    // label is set in UpdateToolbarSidebarText()

    auto* treeCtrl = new TreeCtrl(win->hwndTocBox);
    treeCtrl->fullRowSelect = true;
    treeCtrl->dwExStyle = WS_EX_STATICEDGE;
    treeCtrl->onGetTooltip = TocCustomizeTooltip;
    treeCtrl->onContextMenu = TocContextMenu;
    treeCtrl->onChar = TocTreeCharHandler;
    treeCtrl->onMouseWheel = TocTreeMouseWheelHandler;
    treeCtrl->onTreeSelectionChanged = TocTreeSelectionChanged;
    treeCtrl->onTreeClick = TocTreeClick;
    treeCtrl->onTreeKeyDown = TocTreeKeyDown;

    // TODO: leaks font?
    HFONT fnt = GetTreeFont();
    treeCtrl->SetFont(fnt);

    bool ok = treeCtrl->Create();
    CrashIf(!ok);
    win->tocTreeCtrl = treeCtrl;
    SubclassToc(win);
}
