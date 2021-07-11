package main

import (
	"bytes"
	"html/template"
	"io/fs"
	"io/ioutil"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/dustin/go-humanize"
	"github.com/kjk/u"
)

const crashesPrefix = "updatecheck/uploadedfiles/sumatrapdf-crashes/"
const filterNoSymbols = true

// we don't want want to show crsahes for outdated builds
// so this is usually set to the latest pre-release build
// https://www.sumatrapdfreader.org/prerelease.html
const filterOlderVersions = true
const lowestCrashingBuildToShow = 13598

const nDaysToKeep = 14

// apparently those are forks of sumatra, crash a lot
// and didn't bother to turn off crash reporting
var blacklistedCrashes = []string{
	"huizhipdf.exe",
	"einkreader.exe",
	"jikepdf.exe",
	"JiKepdf.exe",
	"scpdfviewer.exe",
	"Ver: 3.3 (dbg)",
	"Ver: 3.2",
	"Ver: 3.2 64-bit",
	"Ver: 3.3 64-bit (dbg)",
	"nviewh64.dll!DestroyThreadFeatureManager",
}

var (
	// maps day key as YYYY-MM-DD to a list of crashInfo
	crashesPerDay   = map[string][]*crashInfo{}
	muCrashesPerDay sync.Mutex
	nRemoteFiles    = 0
	nWritten        = 0
	tmplCrashParsed *template.Template
	tmplDayParsed   *template.Template
)

type crashVersion struct {
	Main         string
	Build        int
	IsPreRelease bool
	Is64bit      bool
}

type crashLine struct {
	Text string
	URL  string
}

type crashInfo struct {
	N                int
	Day              string // yy-mm-dd format
	Version          string
	VersionDisp      string
	Ver              *crashVersion
	GitSha1          string
	CrashFile        string
	OS               string
	CrashLines       []string
	CrashLinesLinked []crashLine
	crashLinesAll    string
	ExceptionInfo    []string
	exceptionInfoAll string

	// soft-delete
	isDeleted bool

	// path of a file on disk, if exists
	pathTxt  string
	fileSize int

	// unique name of the crash
	fileNameHTML string
	fileNameTxt  string
	// full key in remote store
	storeKey string
}

func (ci *crashInfo) ShortCrashLine() string {
	if len(ci.CrashLines) == 0 {
		return "(none)"
	}
	s := ci.CrashLines[0]
	// "000000007791A365 01:0000000000029365 ntdll.dll!RtlFreeHeap+0x1a5" => "dll!RtlFreeHeap+0x1a5"
	parts := strings.Split(s, " ")
	if len(parts) <= 2 {
		return s
	}
	return strings.Join(parts[2:], " ")
}

func (ci *crashInfo) CrashLine() string {
	if len(ci.CrashLines) == 0 {
		return "(none)"
	}
	return ci.CrashLines[0]
}

func (ci *crashInfo) URL() string {
	return "/" + ci.fileNameHTML
}

func symbolicateCrashLine(s string, gitSha1 string) crashLine {
	if strings.HasPrefix(s, "GetStackFrameInfo()") {
		return crashLine{
			Text: s,
		}
	}
	parts := strings.SplitN(s, " ", 4)
	if len(parts) < 2 {
		return crashLine{
			Text: s,
		}
	}
	parts = parts[2:]
	text := strings.Join(parts, " ")
	uri := ""
	if len(parts) == 2 {
		s = parts[1]
		// D:\a\sumatrapdf\sumatrapdf\src\EbookController.cpp+329
		idx := strings.LastIndex(s, `\sumatrapdf\`)
		if idx > 0 {
			s = s[idx+len(`\sumatrapdf\`):]
			// src\EbookController.cpp+329
			parts = strings.Split(s, "+")
			// https://github.com/sumatrapdfreader/sumatrapdf/blob/67e5328b235fa3d5c23622bc8f05f43865fa03f8/.gitattributes#L2
			line := ""
			filePath := parts[0]
			if len(parts) == 2 {
				line = parts[1]
			}
			filePath = strings.Replace(filePath, "\\", "/", -1)
			uri = "https://github.com/sumatrapdfreader/sumatrapdf/blob/" + gitSha1 + "/" + filePath + "#L" + line
		}
	}
	res := crashLine{
		Text: text,
		URL:  uri,
	}
	return res
}

func symbolicateCrashInfoLines(ci *crashInfo) {
	for _, s := range ci.CrashLines {
		cl := symbolicateCrashLine(s, ci.GitSha1)
		ci.CrashLinesLinked = append(ci.CrashLinesLinked, cl)
	}
}

/*
given:
Ver: 3.2.11495 pre-release 64-bit
produces:
	main: "3.2"
	build: 11495
	isPreRelease: true
	is64bit: tru
*/
func parseCrashVersion(line string) *crashVersion {
	s := strings.TrimPrefix(line, "Ver: ")
	s = strings.TrimSpace(s)
	parts := strings.Split(s, " ")
	res := &crashVersion{}
	v := parts[0] // 3.2.11495
	for _, s = range parts[1:] {
		if s == "pre-release" {
			res.IsPreRelease = true
			continue
		}
		if s == "64-bit" {
			res.Is64bit = true
		}
	}
	// v is like: "3.2.11495 (dbg)"
	parts = strings.Split(v, ".")
	switch len(parts) {
	case 3:
		build, err := strconv.Atoi(parts[2])
		must(err)
		res.Build = build
	case 2:
		res.Main = parts[0] + "." + parts[1]
	default:
		// shouldn't happen but sadly there are badly generated crash reports
		res.Main = v
	}
	return res
}

func isEmptyLine(s string) bool {
	return len(strings.TrimSpace((s))) == 0
}

func removeEmptyLines(a []string) []string {
	var res []string
	for _, s := range a {
		s = strings.TrimSpace(s)
		if len(s) > 0 {
			res = append(res, s)
		}
	}
	return res
}

func removeEmptyCrashLines(a []string) []string {
	var res []string
	for _, s := range a {
		s = strings.TrimSpace(s)
		if strings.Contains(s, " ") || strings.Contains(s, ".") {
			res = append(res, s)
			continue
		}
		if len(s) == len("0000000000000001") || len(s) == len("0000003F") {
			continue
		}
		res = append(res, s)
	}
	return res
}

func parseCrash(d []byte, ci *crashInfo) {
	d = u.NormalizeNewlines(d)
	s := string(d)
	lines := strings.Split(s, "\n")
	var tmpLines []string
	inExceptionInfo := false
	inCrashLines := false
	for _, l := range lines {
		if inExceptionInfo {
			if isEmptyLine(s) || len(tmpLines) > 5 {
				ci.ExceptionInfo = removeEmptyLines(tmpLines)
				tmpLines = nil
				inExceptionInfo = false
				continue
			}
			tmpLines = append(tmpLines, l)
			continue
		}
		if inCrashLines {
			if isEmptyLine(s) || strings.HasPrefix(s, "Thread:") || len(tmpLines) > 32 {
				ci.CrashLines = removeEmptyCrashLines(tmpLines)
				tmpLines = nil
				inCrashLines = false
				continue
			}
			tmpLines = append(tmpLines, l)
			continue
		}
		if strings.HasPrefix(l, "Crash file:") {
			ci.CrashFile = l
			continue
		}
		if strings.HasPrefix(l, "OS:") {
			ci.OS = l
			continue
		}
		if strings.HasPrefix(l, "Git:") {
			parts := strings.Split(l, " ")
			ci.GitSha1 = parts[1]
			continue
		}
		if strings.HasPrefix(l, "Exception:") {
			inExceptionInfo = true
			tmpLines = []string{l}
			continue
		}
		if strings.HasPrefix(l, "Crashed thread:") {
			inCrashLines = true
			tmpLines = nil
			continue
		}
		if strings.HasPrefix(l, "Ver:") {
			ci.Version = l
			ci.Ver = parseCrashVersion(l)
		}
	}
	ci.crashLinesAll = strings.Join(ci.CrashLines, "\n")
	ci.exceptionInfoAll = strings.Join(ci.ExceptionInfo, "\n")
	symbolicateCrashInfoLines(ci)
}

var crashesDirCached = ""

func crashesDataDir() string {
	if crashesDirCached != "" {
		return crashesDirCached
	}
	dir := u.UserHomeDirMust()
	dir = filepath.Join(dir, "data", "sumatra-crashes")
	u.CreateDirMust((dir))
	crashesDirCached = dir
	return dir
}

var crashesHTMLDirCached = ""

func crashesHTMLDataDir() string {
	if crashesHTMLDirCached != "" {
		return crashesHTMLDirCached
	}
	dir := u.UserHomeDirMust()
	dir = filepath.Join(dir, "data", "sumatra-crashes-html")
	// we want to empty this dir every time
	must(os.RemoveAll(dir))
	u.CreateDirMust((dir))
	crashesHTMLDirCached = dir
	return dir
}

// convert YYYY/MM/DD => YYYY-MM-DD
func storeDayToDirDay(s string) string {
	parts := strings.Split(s, "/")
	panicIf(len(parts) != 3)
	day := strings.Join(parts, "-")
	panicIf(len(day) != 10) // "yyyy-dd-mm"
	return day
}

// return true if crash in that day is outdated
func isOutdated(day string) bool {
	d1, err := time.Parse("2006-01-02", day)
	panicIfErr(err)
	diff := time.Since(d1)
	return diff > time.Hour*24*nDaysToKeep
}

// decide if should delete crash based on the content
// this is to filter out crashes we don't care about
// like those from other apps
func shouldDeleteParsedCrash(body []byte) bool {
	for _, s := range blacklistedCrashes {
		if bytes.Contains(body, []byte(s)) {
			return true
		}
	}
	return false
}

const nDownloaders = 64

var (
	nInvalid    = 0
	nReadFiles  = 0
	nDownloaded = 0
	nDeleted    = 0
)

// retry 3 times because processing on GitHub actions often fails due to
// "An existing connection was forcibly closed by the remote host"
func downloadAtomicallyRetry(mc *u.MinioClient, path string, key string) {
	var err error
	for i := 0; i < 3; i++ {
		err = mc.DownloadFileAtomically(path, key)
		if err == nil {
			return
		}
		logf("Downloading '%s' to '%s' failed with '%s'\n", key, path, err)
		time.Sleep(time.Millisecond * 500)
	}
	panicIf(true, "mc.DownloadFileAtomically('%s', '%s') failed with '%s'", path, key, err)
}

func downloadOrReadOrDelete(mc *u.MinioClient, ci *crashInfo) {
	dataDir := crashesDataDir()
	path := filepath.Join(dataDir, ci.Day, ci.fileNameTxt)
	ci.pathTxt = path

	if isOutdated(ci.Day) {
		ci.isDeleted = true
		mc.Delete(ci.storeKey)
		os.Remove(path)
		nDeleted++
		if nDeleted < 32 || nDeleted%100 == 0 {
			logf("deleted outdated %s %d\n", ci.storeKey, nRemoteFiles)
		}
		return
	}

	body, err := ioutil.ReadFile(path)
	if err == nil {
		nReadFiles++
		if nReadFiles < 32 || nReadFiles%200 == 0 {
			logf("read %s for %s %d\n", path, ci.storeKey, nRemoteFiles)
		}
	} else {
		downloadAtomicallyRetry(mc, path, ci.storeKey)
		body, err = ioutil.ReadFile(path)
		panicIfErr(err)
		nDownloaded++
		if nDownloaded < 50 || nDownloaded%200 == 0 {
			logf("downloaded '%s' => '%s' %d\n", ci.storeKey, path, nRemoteFiles)
		}
	}
	ci.fileSize = len(body)

	parseCrash(body, ci)
	if shouldDeleteParsedCrash(body) {
		ci.isDeleted = true
		mc.Delete(ci.storeKey)
		os.Remove(path)
		nInvalid++
		if nInvalid < 32 || nInvalid%100 == 0 {
			logf("deleted invalid %s %d\n", path, nRemoteFiles)
		}
		return
	}
	if filterNoSymbols {
		// those are not hard deleted but we don't want to show them
		// TODO: maybe delete them as well?
		if hasNoSymbols(ci) {
			ci.isDeleted = true
		}
	}
	if filterOlderVersions && ci.Ver != nil {
		build := ci.Ver.Build
		// filter out outdated builds
		if build > 0 && build <= lowestCrashingBuildToShow {
			ci.isDeleted = true
		}

	}
}

// if crash line ends with ".exe", we assume it's because it doesn't
// have symbols
func hasNoSymbols(ci *crashInfo) bool {
	s := ci.CrashLine()
	return strings.HasSuffix(s, ".exe")
}

func previewCrashes() {
	panicIf(!hasSpacesCreds())
	if true {
		panicIf(os.Getenv("NETLIFY_AUTH_TOKEN") == "", "missing NETLIFY_AUTH_TOKEN env variable")
		panicIf(os.Getenv("NETLIFY_SITE_ID") == "", "missing NETLIFY_SITE_ID env variable")
	}
	dataDir := crashesDataDir()
	logf("previewCrashes: data dir: '%s'\n", dataDir)

	var wg sync.WaitGroup
	wg.Add(1)
	// semaphore for limiting concurrent downloaders
	sem := make(chan bool, nDownloaders)
	go func() {
		mc := newMinioClient()
		client, err := mc.GetClient()
		must(err)

		doneCh := make(chan struct{})
		defer close(doneCh)

		remoteFiles := client.ListObjects(mc.Bucket, crashesPrefix, true, doneCh)

		finished := false
		for rf := range remoteFiles {
			if finished {
				continue
			}
			nRemoteFiles++
			must(rf.Err)
			//logf("key: %s\n", rf.Key)

			wg.Add(1)
			sem <- true
			go func(key string) {
				s := strings.TrimPrefix(key, crashesPrefix)
				fileNameTxt := path.Base(s)
				// now day is YYYY/MM/DD/rest
				day := path.Dir(s)
				day = storeDayToDirDay(day)
				crash := &crashInfo{
					Day:         day,
					fileNameTxt: fileNameTxt,
					// foo.txt => foo.html
					fileNameHTML: strings.Replace(fileNameTxt, ".txt", ".html", -1),
				}
				crash.storeKey = key
				downloadOrReadOrDelete(mc, crash)
				if !crash.isDeleted {
					muCrashesPerDay.Lock()
					crashes := crashesPerDay[day]
					crashes = append(crashes, crash)
					crashesPerDay[day] = crashes
					muCrashesPerDay.Unlock()
				}
				<-sem
				wg.Done()
			}(rf.Key)
		}
		wg.Done()
	}()
	wg.Wait()

	logf("%d remote files, %d invalid\n", nRemoteFiles, nInvalid)
	filterDeletedCrashes()
	filterBigCrashes()

	days := getDaysSorted()
	for idx, day := range days {
		a := crashesPerDay[day]
		logf("%s: %d\n", day, len(a))
		sort.Slice(a, func(i, j int) bool {
			v1 := a[i].Version
			v2 := a[j].Version
			if v1 == v2 {
				c1 := getFirstString(a[i].CrashLines)
				c2 := getFirstString(a[j].CrashLines)
				if len(c1) == len(c2) {
					return c1 < c2
				}
				return len(c1) > len(c2)
			}
			return v1 > v2
		})
		crashesPerDay[day] = a

		if false && idx == 0 {
			for _, ci := range a {
				logf("%s %s\n", ci.Version, getFirstString(ci.CrashLines))
			}
		}
	}
	genCrashesHTML()
	if false {
		// using https://github.com/netlify/cli
		cmd := exec.Command("netlify", "dev", "-p", "8765", "--dir", ".")
		cmd.Dir = crashesHTMLDataDir()
		u.RunCmdLoggedMust(cmd)
	}
	if true {
		cmd := exec.Command("netlify", "deploy", "--prod", "--dir", ".")
		if strings.Contains(runtime.GOOS, "windows") {
			// if on windows assume running locally so open browser
			// automatically after deploying
			cmd = exec.Command("netlify", "deploy", "--prod", "--open", "--dir", ".")
		}
		cmd.Dir = crashesHTMLDataDir()
		u.RunCmdLoggedMust(cmd)
	}
}

func dirSize(dir string) {
	totalSize := int64(0)
	fun := func(path string, info fs.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		totalSize += info.Size()
		return nil
	}
	filepath.Walk(dir, fun)
	logf("Size of dir %s: %s\n", dir, humanize.Bytes(uint64(totalSize)))
}

const tmplDay = `
<!doctype html>
<html>
<head>
<style>
		html, body {
				font-family: monospace;
				font-size: 10pt;
				margin: 1em;
				padding: 0;
		}
		.td1 {
			white-space: nowrap;
			padding-right: 1em;
		}
		.tdh {
			text-align: center;
		}
</style>
</head>
<body>
	<p>
		{{range .Days}}<a href="/{{.}}.html" target="_blank">{{.}}</a>&nbsp;&nbsp;{{end}}
	</p>
	<p>
		<table>
			<thead>
				<tr>
					<td class="tdh">ver</td>
					<td class="tdh">crash</td>
				</tr>
			</thead>
			<tbody>
				{{range .CrashSummaries}}
				<tr>
					<td class="td1">{{.VersionDisp}}</td>
					<td><a href="{{.URL}}" target="_blank">{{.ShortCrashLine}}</a></td>
				</tr>
				{{end}}
			</tbody>
		</table>
	</p>
</body>
</html>
`

const tmplCrash = `
<!doctype html>
<httml>
    <head>
        <style>
            html, body {
                font-family: monospace;
                font-size: 10pt;
								margin: 1em;
								padding: 0;
								}
        </style>
    </head>
    <body>
        <div style="padding-bottom: 1em;"><a href="/">All crashes</a></div>
            {{range .CrashLinesLinked}}
                {{if .URL}}
                    <div><a href="{{.URL}}" target="_blank">{{.Text}}</a></div>
                {{else}}
                    <div>{{.Text}}</div>
                {{end}}
            {{end}}
        <pre>{{.CrashBody}}</pre>
    </body>
</html>
`

func getTmplCrash() *template.Template {
	if tmplCrashParsed == nil {
		tmplCrashParsed = template.Must(template.New("t2").Parse(tmplCrash))
	}
	return tmplCrashParsed
}

func getTmplDay() *template.Template {
	if tmplDayParsed == nil {
		tmplDayParsed = template.Must(template.New("t1").Parse(tmplDay))
	}
	return tmplDayParsed
}

func genCrashHTML(dir string, ci *crashInfo) {
	name := ci.fileNameHTML
	path := filepath.Join(dir, name)
	body := u.ReadFileMust(ci.pathTxt)
	var buf bytes.Buffer
	v := struct {
		CrashBody        string
		CrashLinesLinked []crashLine
	}{
		CrashLinesLinked: ci.CrashLinesLinked,
		CrashBody:        string(body),
	}
	err := getTmplCrash().Execute(&buf, v)
	must(err)
	d := buf.Bytes()

	u.WriteFileMust(path, d)
	nWritten++
	if nWritten < 8 || nWritten%100 == 0 {
		logf("wrote %s %d\n", path, nWritten)
	}
}

func genCrashesHTMLForDay(dir string, day string, isIndex bool) {
	days := getDaysSorted()
	path := filepath.Join(dir, day+".html")

	a := crashesPerDay[day]
	prevVer := ""
	for _, ci := range a {
		ver := ci.Version
		if ver != prevVer {
			ver = strings.TrimPrefix(ver, "Ver: ")
			ver = strings.TrimSpace(ver)
			ci.VersionDisp = ver
			prevVer = ci.Version
		}
	}

	v := struct {
		Days           []string
		CrashSummaries []*crashInfo
	}{
		Days:           days,
		CrashSummaries: a,
	}

	var buf bytes.Buffer
	err := getTmplDay().Execute(&buf, v)
	must(err)
	d := buf.Bytes()

	u.WriteFileMust(path, d)
	if isIndex {
		path = filepath.Join(dir, "index.html")
		u.WriteFileMust(path, d)
	}

	for _, ci := range a {
		genCrashHTML(dir, ci)
	}
}

func genCrashesHTML() {
	dir := crashesHTMLDataDir()
	logf("genCrashesHTML in %s\n", dir)
	days := getDaysSorted()
	for idx, day := range days {
		genCrashesHTMLForDay(dir, day, idx == 0)
	}
	dirSize(dir)
}

func getFirstString(a []string) string {
	if len(a) == 0 {
		return "(none)"
	}
	return a[0]
}

func filterDeletedCrashes() {
	nFiltered := 0
	for day, a := range crashesPerDay {
		var filtered []*crashInfo
		for _, ci := range a {
			if ci.isDeleted {
				nFiltered++
				continue
			}
			filtered = append(filtered, ci)
		}
		crashesPerDay[day] = filtered
	}
	logf("filterDeletedCrashes: filtered %d\n", nFiltered)
}

// to avoid uploading too much to netlify, we filter
// crashes from 3.2 etc. that are 256kb or more in size
func filterBigCrashes() {
	nFiltered := 0
	for day, a := range crashesPerDay {
		var filtered []*crashInfo
		for _, ci := range a {
			if ci.Ver == nil {
				nFiltered++
				continue
			}
			// TODO: maybe delete those crashes in main loop and apply to all versions
			if ci.fileSize > 256*1024 && ci.Ver.Main == "3.2" {
				nFiltered++
				continue
			}
			filtered = append(filtered, ci)
		}
		crashesPerDay[day] = filtered
	}
	logf("filterBigCrashes: filtered %d\n", nFiltered)
}

func getDaysSorted() []string {
	var days []string
	for day := range crashesPerDay {
		days = append(days, day)
	}
	sort.Sort(sort.Reverse(sort.StringSlice(days)))
	return days
}
