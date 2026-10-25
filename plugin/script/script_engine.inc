// script_engine.inc - included by overlay.inc (inside namespace ov)
//
// Runs the click scripts produced by tools/script_editor.py. This is the only
// thing the overlay button does: there is no hard-coded click chain any more.
//
// File:      %APPDATA%\TTVoice\scripts\<name>.json
// Selection: overlay.ini "script=<name>" wins; otherwise the name the editor
//            wrote to active.txt on save; otherwise the newest *.json.
// Trigger:   overlay button click, or the script's own hotkey when it has one.
// Reload:    the last-write time is checked before every run, so saving in the
//            editor is enough - no app restart, no redeploy.
//
// Schema (see tools/script_editor.py for the authoritative description):
//   {"version":1,"name":"...","hotkey":"F1","loop":1,
//    "events":[{"x":27,"y":113,"ox":0,"oy":0,"click":"left","delay_ms":600,
//               "wheel":0,"remark":""}, ...]}
// Coordinates are logical (96 dpi) view-relative, exactly like overlay.ini.
// x/y is the first coordinate (the base, the same values on every row); ox/oy
// is the row's offset from it (row 1 has 0,0). The executed point is
// (x + ox, y + oy) - the engine folds the offsets into x/y right after the
// parse, so old files without ox/oy keep clicking their absolute x/y.
//
// Optional per-event "selector" (CSS selector string): the element is located
// in the H5 page over the WebView2 DevTools endpoint (armed by the proxy at
// process start, see the CDP section below) and clicked there with
// Input.dispatchMouseEvent - events enter Chromium's trusted input pipeline,
// the same one OS mouse messages take, and need no foreground window. Optional
// script-level "target_url" picks the page when several are open (substring
// match on the page URL, UTF-8). A selector row that misses (no endpoint, no
// element) falls back to the recorded x/y SendInput click.
//
// Keep this file pure ASCII.

static const int kScriptMaxEvents = 32;
static const int kScriptText      = 256;
static const int kScriptHotkeyId  = 0x5454;  // "TT"

struct ScriptEvent {
    float   x = 0.0f;        // first coordinate (base); folded with ox/oy below
    float   y = 0.0f;
    float   ox = 0.0f;       // offset from the base (JSON "ox")
    float   oy = 0.0f;       // offset from the base (JSON "oy")
    int     click = 0;       // 0 = left, 1 = double, 2 = right
    int     delayMs = 0;     // wait after this row
    int     wheel = 0;       // notches, negative scrolls down
    wchar_t text[kScriptText] = {};
    char    selector[128] = {};   // CSS selector: click the element over CDP
};

struct Script {
    wchar_t     name[64] = {};
    int         loop = 1;
    int         hotkeyVk = 0;
    int         count = 0;
    int         viewW = 0;               // editor's view_size record (physical px)
    int         viewH = 0;
    char        targetUrl[160] = {};     // JSON "target_url": pick page by URL substring
    ScriptEvent events[kScriptMaxEvents];
};

static Script    g_script;                       // last parsed script
static wchar_t   g_scriptPath[MAX_PATH] = {};    // file g_script came from
static ULONGLONG g_scriptStamp = 0;              // last-write time of that file
static bool      g_scriptSeen = false;
static bool      g_loggedNoScript = false;    // one-shot guard for the 2s poll log
static char      g_scriptErr[64] = {};        // last load failure, so a click can repeat it
static wchar_t   g_scriptName[64] = {};          // overlay.ini "script="
static volatile LONG g_scriptActive = 0;
static volatile LONG g_scriptRuns = 0;
static SRWLOCK   g_scriptLock = SRWLOCK_INIT;    // guards g_script vs. reload

// ---------------------------------------------------------------------------
// Small JSON reader (just enough for the editor's output)
// ---------------------------------------------------------------------------

struct JsonScan {
    const char* p = nullptr;
    const char* end = nullptr;
};

static void JsonWs(JsonScan* s) {
    while (s->p < s->end) {
        const char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++s->p;
            continue;
        }
        break;
    }
}

// Appends one code point as UTF-8, keeping room for the terminator.
static void JsonUtf8Put(char* out, int cap, int* len, unsigned cp) {
    int need = 0;
    if (cp < 0x80) need = 1;
    else if (cp < 0x800) need = 2;
    else if (cp < 0x10000) need = 3;
    else if (cp <= 0x10FFFF) need = 4;
    else return;
    if (*len + need > cap - 1) return;

    char* d = out + *len;
    if (need == 1) {
        d[0] = static_cast<char>(cp);
    } else if (need == 2) {
        d[0] = static_cast<char>(0xC0 | (cp >> 6));
        d[1] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (need == 3) {
        d[0] = static_cast<char>(0xE0 | (cp >> 12));
        d[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        d[2] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        d[0] = static_cast<char>(0xF0 | (cp >> 18));
        d[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        d[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        d[3] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    *len += need;
}

static int JsonHex4(const char* p) {
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else return -1;
    }
    return v;
}

// Reads a JSON string (cursor on the opening quote) into out as UTF-8.
static bool JsonString(JsonScan* s, char* out, int cap) {
    if (s->p >= s->end || *s->p != '"') return false;
    if (cap <= 0) return false;
    out[0] = 0;
    ++s->p;

    int len = 0;
    while (s->p < s->end) {
        const char c = *s->p++;
        if (c == '"') {
            out[len] = 0;
            return true;
        }
        if (c != '\\') {
            JsonUtf8Put(out, cap, &len, static_cast<unsigned char>(c));
            continue;
        }
        if (s->p >= s->end) return false;
        const char e = *s->p++;
        switch (e) {
        case 'n': JsonUtf8Put(out, cap, &len, '\n'); break;
        case 't': JsonUtf8Put(out, cap, &len, '\t'); break;
        case 'r': JsonUtf8Put(out, cap, &len, '\r'); break;
        case 'b': JsonUtf8Put(out, cap, &len, '\b'); break;
        case 'f': JsonUtf8Put(out, cap, &len, '\f'); break;
        case 'u': {
            if (s->end - s->p < 4) return false;
            unsigned cp = static_cast<unsigned>(JsonHex4(s->p));
            s->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && s->end - s->p >= 6 &&
                s->p[0] == '\\' && s->p[1] == 'u') {
                const int lo = JsonHex4(s->p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (static_cast<unsigned>(lo) - 0xDC00u);
                    s->p += 6;
                }
            }
            JsonUtf8Put(out, cap, &len, cp);
            break;
        }
        default:
            JsonUtf8Put(out, cap, &len, static_cast<unsigned char>(e));
            break;
        }
    }
    return false;  // unterminated
}

static bool JsonNumber(JsonScan* s, double* out) {
    const char* start = s->p;
    while (s->p < s->end) {
        const char c = *s->p;
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
            c == 'e' || c == 'E') {
            ++s->p;
            continue;
        }
        break;
    }
    const size_t n = static_cast<size_t>(s->p - start);
    if (n == 0 || n >= 40) return false;
    char tmp[40] = {};
    memcpy(tmp, start, n);
    *out = atof(tmp);
    return true;
}

// Skips any value; strings are skipped without decoding.
static void JsonSkip(JsonScan* s) {
    JsonWs(s);
    if (s->p >= s->end) return;
    const char c = *s->p;

    if (c == '"') {
        char dump[2] = {};
        JsonString(s, dump, 1);
        return;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        while (s->p < s->end) {
            const char d = *s->p;
            if (d == '"') {
                char dump[2] = {};
                JsonString(s, dump, 1);
                continue;
            }
            ++s->p;
            if (d == '{' || d == '[') ++depth;
            else if (d == '}' || d == ']') {
                if (--depth <= 0) return;
            }
        }
        return;
    }
    while (s->p < s->end) {
        const char d = *s->p;
        if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' ||
            d == '\r' || d == '\n') {
            return;
        }
        ++s->p;
    }
}

static int ClickFromName(const char* name) {
    if (_stricmp(name, "double") == 0) return 1;
    if (_stricmp(name, "right") == 0) return 2;
    return 0;
}

// "F1".."F24", a single letter or a single digit; anything else = no hotkey.
static int HotkeyFromName(const char* name) {
    if (!name || !name[0]) return 0;
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        const int n = atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
        return 0;
    }
    if (name[1] != 0) return 0;
    const char c = name[0];
    if (c >= 'A' && c <= 'Z') return c;
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= '0' && c <= '9') return c;
    return 0;
}

// MultiByteToWideChar with truncation instead of failure.
static void Utf8ToWideN(const char* s, wchar_t* dst, int cap) {
    if (cap <= 0) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, dst, cap);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, s, -1, dst, cap);
    if (n <= 0) dst[0] = 0;
    dst[cap - 1] = 0;
}

// Wide -> UTF-8 into a caller buffer, for log lines.
static const char* Nz(const wchar_t* src, char* dst, int cap) {
    if (cap <= 0) return "";
    dst[0] = 0;
    if (src) WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, cap - 1, nullptr, nullptr);
    dst[cap - 1] = 0;
    return dst;
}

static bool JsonParseEvent(JsonScan* s, ScriptEvent* ev) {
    if (s->p >= s->end || *s->p != '{') return false;
    ++s->p;

    while (s->p < s->end) {
        JsonWs(s);
        if (s->p < s->end && *s->p == '}') {
            ++s->p;
            return true;
        }
        char key[32] = {};
        if (!JsonString(s, key, sizeof(key))) return false;
        JsonWs(s);
        if (s->p >= s->end || *s->p != ':') return false;
        ++s->p;
        JsonWs(s);

        if (_stricmp(key, "x") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->x = static_cast<float>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "y") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->y = static_cast<float>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "ox") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->ox = static_cast<float>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "oy") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->oy = static_cast<float>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "click") == 0) {
            char kind[16] = {};
            if (JsonString(s, kind, sizeof(kind))) ev->click = ClickFromName(kind);
            else JsonSkip(s);
        } else if (_stricmp(key, "delay_ms") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->delayMs = static_cast<int>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "wheel") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->wheel = static_cast<int>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "text") == 0) {
            char text[1024] = {};
            if (JsonString(s, text, sizeof(text))) Utf8ToWideN(text, ev->text, kScriptText);
            else JsonSkip(s);
        } else if (_stricmp(key, "selector") == 0) {
            if (!JsonString(s, ev->selector, sizeof(ev->selector))) JsonSkip(s);
        } else {
            JsonSkip(s);
        }

        JsonWs(s);
        if (s->p < s->end && *s->p == ',') ++s->p;
    }
    return false;
}

static bool JsonParseScript(const char* text, size_t len, Script* out) {
    *out = Script();

    JsonScan s;
    s.p = text;
    s.end = text + len;
    JsonWs(&s);
    if (s.p >= s.end || *s.p != '{') return false;
    ++s.p;

    while (s.p < s.end) {
        JsonWs(&s);
        if (s.p < s.end && *s.p == '}') break;

        char key[32] = {};
        if (!JsonString(&s, key, sizeof(key))) return false;
        JsonWs(&s);
        if (s.p >= s.end || *s.p != ':') return false;
        ++s.p;
        JsonWs(&s);

        if (_stricmp(key, "events") == 0) {
            if (s.p < s.end && *s.p == '[') {
                ++s.p;
                while (s.p < s.end) {
                    JsonWs(&s);
                    if (s.p < s.end && *s.p == ']') {
                        ++s.p;
                        break;
                    }
                    if (s.p < s.end && *s.p == '{') {
                        if (out->count < kScriptMaxEvents) {
                            if (JsonParseEvent(&s, &out->events[out->count])) ++out->count;
                            else return false;
                        } else {
                            JsonSkip(&s);
                        }
                    } else {
                        JsonSkip(&s);
                    }
                    JsonWs(&s);
                    if (s.p < s.end && *s.p == ',') ++s.p;
                }
            } else {
                JsonSkip(&s);
            }
        } else if (_stricmp(key, "loop") == 0) {
            double v = 0;
            if (JsonNumber(&s, &v)) {
                out->loop = (v >= 1.0) ? static_cast<int>(v) : 1;
                if (out->loop > 99) out->loop = 99;
            } else {
                JsonSkip(&s);
            }
        } else if (_stricmp(key, "hotkey") == 0) {
            char hk[32] = {};
            if (JsonString(&s, hk, sizeof(hk))) out->hotkeyVk = HotkeyFromName(hk);
            else JsonSkip(&s);
        } else if (_stricmp(key, "name") == 0) {
            char nm[128] = {};
            if (JsonString(&s, nm, sizeof(nm))) Utf8ToWideN(nm, out->name, 64);
            else JsonSkip(&s);
        } else if (_stricmp(key, "target_url") == 0) {
            if (!JsonString(&s, out->targetUrl, sizeof(out->targetUrl))) JsonSkip(&s);
        } else if (_stricmp(key, "view_size") == 0) {
            // Editor records the view client size the coordinates were picked
            // at; the plugin warns when it differs at run time (centered H5
            // popups lay out differently per window size).
            if (s.p < s.end && *s.p == '[') {
                ++s.p;
                int idx = 0;
                while (s.p < s.end) {
                    JsonWs(&s);
                    if (s.p < s.end && *s.p == ']') {
                        ++s.p;
                        break;
                    }
                    double v = 0;
                    if (JsonNumber(&s, &v)) {
                        if (idx == 0) out->viewW = static_cast<int>(v);
                        else if (idx == 1) out->viewH = static_cast<int>(v);
                    } else {
                        JsonSkip(&s);
                    }
                    ++idx;
                    JsonWs(&s);
                    if (s.p < s.end && *s.p == ',') ++s.p;
                }
            } else {
                JsonSkip(&s);
            }
        } else {
            JsonSkip(&s);
        }

        JsonWs(&s);
        if (s.p < s.end && *s.p == ',') ++s.p;
    }

    // The executed point is (x + ox, y + oy): fold the offsets into x/y once
    // so every runner (plugin steps and the engine thread) just uses x/y.
    // Files without ox/oy sum 0 and keep clicking their absolute x/y.
    for (int i = 0; i < out->count; ++i) {
        out->events[i].x += out->events[i].ox;
        out->events[i].y += out->events[i].oy;
    }

    return out->count > 0;
}

// ---------------------------------------------------------------------------
// File discovery / reload
// ---------------------------------------------------------------------------

static bool ScriptDir(wchar_t* out, int cap) {
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return false;
    swprintf_s(out, cap, L"%s\\TTVoice\\scripts", appdata);
    return true;
}

static bool ScriptReadFile(const wchar_t* path, Script* out, char* err, size_t errCap) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) {
        strcpy_s(err, errCap, "open failed");
        return false;
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) {
        fclose(fp);
        strcpy_s(err, errCap, "bad size");
        return false;
    }

    char* buf = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
    if (!buf) {
        fclose(fp);
        strcpy_s(err, errCap, "out of memory");
        return false;
    }
    const size_t got = fread(buf, 1, static_cast<size_t>(size), fp);
    fclose(fp);
    buf[got] = 0;

    const char* text = buf;
    if (got >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text += 3;  // UTF-8 BOM
    }

    const bool ok = JsonParseScript(text, got - static_cast<size_t>(text - buf), out);
    free(buf);
    if (!ok) strcpy_s(err, errCap, "no events / bad json");
    return ok;
}

// Picks the script to run: ini "script=" > active.txt > newest *.json.
static bool ScriptResolvePath(wchar_t* out, int cap) {
    wchar_t dir[MAX_PATH] = {};
    if (!ScriptDir(dir, MAX_PATH)) return false;

    if (g_scriptName[0] != 0) {
        const wchar_t* tail = wcsstr(g_scriptName, L".json") ? L"" : L".json";
        swprintf_s(out, cap, L"%s\\%s%s", dir, g_scriptName, tail);
        return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
    }

    wchar_t act[MAX_PATH] = {};
    swprintf_s(act, L"%s\\active.txt", dir);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, act, L"rb") == 0 && fp != nullptr) {
        char buf[160] = {};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        buf[n] = 0;
        char* e = buf;
        while (*e && *e != '\r' && *e != '\n') ++e;
        *e = 0;
        if (buf[0]) {
            wchar_t name[160] = {};
            Utf8ToWideN(buf, name, 160);
            swprintf_s(out, cap, L"%s\\%s.json", dir, name);
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;
        }
    }

    wchar_t pattern[MAX_PATH] = {};
    swprintf_s(pattern, L"%s\\*.json", dir);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    ULONGLONG best = 0;
    wchar_t bestName[260] = {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG stamp =
            (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
            static_cast<ULONGLONG>(fd.ftLastWriteTime.dwLowDateTime);
        if (stamp >= best) {
            best = stamp;
            wcscpy_s(bestName, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (bestName[0] == 0) return false;
    swprintf_s(out, cap, L"%s\\%s", dir, bestName);
    return true;
}

static ULONGLONG FileStamp(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           static_cast<ULONGLONG>(fad.ftLastWriteTime.dwLowDateTime);
}

// Re-reads the script when the file changed. Cheap: one stat call per run.
//
// verbose = an explicit user action drove this reload (pill click / hotkey
// press). Then the reason for "nothing to run" is printed every single time, so
// a click always explains itself. The 2s poll thread passes false: there the
// "no script" line stays one-shot, otherwise the log would flood twice a minute.
static bool ScriptReload(bool verbose) {
    if (InterlockedCompareExchange(&g_scriptActive, 0, 0) != 0) {
        return g_script.count > 0;  // never swap the script mid-run
    }

    wchar_t path[MAX_PATH] = {};
    if (!ScriptResolvePath(path, MAX_PATH)) {
        if (verbose || !g_loggedNoScript) {
            g_loggedNoScript = true;
            LogFromOverlay("script: no json found, the button does nothing");
        }
        SetScriptStatus(1, nullptr);  // pill shows "no script", the click is a no-op
        g_scriptSeen = false;
        g_script.count = 0;
        g_scriptErr[0] = 0;
        return false;
    }
    g_loggedNoScript = false;

    const ULONGLONG stamp = FileStamp(path);
    if (g_scriptSeen && stamp != 0 && stamp == g_scriptStamp &&
        wcscmp(path, g_scriptPath) == 0) {
        if (verbose && g_script.count <= 0 && g_scriptErr[0]) {
            // Same rejected file as last time: repeat the reason instead of going
            // silent (the parse branch below only fires when the stamp changes).
            char narrow[MAX_PATH * 3] = {};
            char buf[MAX_PATH * 3 + 96] = {};
            sprintf_s(buf, "script: parse failed %s (%s)",
                      Nz(path, narrow, sizeof(narrow)), g_scriptErr);
            LogFromOverlay(buf);
        }
        return g_script.count > 0;
    }

    Script parsed;
    char err[64] = {};
    if (!ScriptReadFile(path, &parsed, err, sizeof(err))) {
        char narrow[MAX_PATH * 3] = {};
        char buf[MAX_PATH * 3 + 96] = {};
        sprintf_s(buf, "script: parse failed %s (%s)", Nz(path, narrow, sizeof(narrow)), err);
        LogFromOverlay(buf);
        SetScriptStatus(2, err);  // pinned: the file is there but unusable
        strcpy_s(g_scriptErr, err);  // remembered so a click can repeat the line
        g_scriptSeen = true;
        g_scriptStamp = stamp;
        wcscpy_s(g_scriptPath, path);
        g_script.count = 0;
        return false;
    }

    AcquireSRWLockExclusive(&g_scriptLock);
    g_script = parsed;
    g_scriptSeen = true;
    g_scriptStamp = stamp;
    wcscpy_s(g_scriptPath, path);
    ReleaseSRWLockExclusive(&g_scriptLock);

    SetScriptStatus(0, nullptr);  // the script owns the button again
    g_scriptErr[0] = 0;           // nothing to explain any more

    {
        char narrow[MAX_PATH * 3] = {};
        char nm[256] = {};
        char buf[MAX_PATH * 3 + 160] = {};
        sprintf_s(buf, "script: loaded %s name=\"%s\" events=%d loop=%d",
                  Nz(path, narrow, sizeof(narrow)), Nz(g_script.name, nm, sizeof(nm)),
                  g_script.count, g_script.loop);
        LogFromOverlay(buf);
    }
    return true;
}

// ---------------------------------------------------------------------------
// WebView2 CDP channel (selector clicks)
//
// The version.dll proxy appends --remote-debugging-port to
// WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS at process start (dllmain.cpp,
// EnableWebView2Cdp), so the host's H5 layer - a WebView2 composited into
// Flutter - exposes the DevTools endpoint on 127.0.0.1. Events with a
// "selector" resolve their element through that endpoint:
//
//   Runtime.evaluate     -> document.querySelector(sel), scrollIntoView if
//                           partly off-screen, getBoundingClientRect in CSS px
//   Input.dispatchMouseEvent -> mouseMoved + mousePressed/mouseReleased at the
//                           rect center, in the same CSS-px viewport coordinates
//
// CDP input events enter Chromium's real input pipeline: the page sees
// isTrusted=true, exactly like the OS mouse messages SendInput produces -
// that is why a click "comes back" as a message-grade event without the
// window being in the foreground or the point being unobstructed. Coordinate
// events keep the SendInput path; a selector row falls back to it only when
// the channel or the element is missing.
//
// Concurrency: only the one active run (engine thread or the plugin's stepped
// runner, never both - g_scriptActive serializes them) may call into here, so
// no extra lock is needed.
//
// Keep this section pure ASCII.
// ---------------------------------------------------------------------------
#pragma comment(lib, "winhttp.lib")
#include <winhttp.h>

static const int kCdpPort = 9223;   // must equal kCdpArg in proxy/dllmain.cpp

static HINTERNET g_cdpSession = nullptr;   // one WinHTTP session per process
static HINTERNET g_cdpConnect = nullptr;   // 127.0.0.1:kCdpPort
static HINTERNET g_cdpWs      = nullptr;   // upgraded websocket to the page
static int       g_cdpMsgId   = 0;
static bool      g_cdpFailed  = false;     // this run already failed to connect

// One connect attempt per run: with the endpoint down, every selector row
// falls straight to its x/y fallback instead of paying the timeout again.
static void CdpRunBegin() { g_cdpFailed = false; }

static void CdpCloseSocket() {
    if (g_cdpWs != nullptr) {
        WinHttpCloseHandle(g_cdpWs);
        g_cdpWs = nullptr;
    }
}

// Called from the plugin's shutdown so a hot reload cannot leak handles.
static void CdpShutdown() {
    CdpCloseSocket();
    if (g_cdpConnect != nullptr) {
        WinHttpCloseHandle(g_cdpConnect);
        g_cdpConnect = nullptr;
    }
    if (g_cdpSession != nullptr) {
        WinHttpCloseHandle(g_cdpSession);
        g_cdpSession = nullptr;
    }
}

// Escapes src into a complete JSON string literal (quotes included).
static void CdpJsonQuote(const char* src, char* dst, int cap) {
    int o = 0;
    dst[o++] = '"';
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
         *p != 0 && o < cap - 8; ++p) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = static_cast<char>(c);
        } else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { dst[o++] = '\\'; dst[o++] = 'r'; }
        else if (c == '\t') { dst[o++] = '\\'; dst[o++] = 't'; }
        else if (c < 0x20) { o += sprintf_s(dst + o, cap - o, "\\u%04x", c); }
        else { dst[o++] = static_cast<char>(c); }   // UTF-8 passes through
    }
    dst[o++] = '"';
    dst[o] = 0;
}

// ASCII case-insensitive substring (for URL matching).
static bool CdpContainsI(const char* hay, const char* needle) {
    if (needle == nullptr || needle[0] == 0) return true;
    if (hay == nullptr) return false;
    const int n = static_cast<int>(strlen(needle));
    for (const char* p = hay; *p != 0; ++p) {
        int i = 0;
        while (i < n && p[i] != 0) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
            if (a != b) break;
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

// Plain HTTP GET on the endpoint (used for /json/list before any websocket).
static bool CdpHttpGet(const wchar_t* path, char* out, int cap) {
    HINTERNET req = WinHttpOpenRequest(g_cdpConnect, L"GET", path, nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (req == nullptr) return false;

    bool ok = false;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        if (WinHttpQueryHeaders(req,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &status, &size, nullptr) &&
            status == 200) {
            int used = 0;
            DWORD avail = 0;
            while (used < cap - 1 &&
                   WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                DWORD want = avail;
                if (want > static_cast<DWORD>(cap - 1 - used)) {
                    want = static_cast<DWORD>(cap - 1 - used);
                }
                DWORD got = 0;
                if (!WinHttpReadData(req, out + used, want, &got) || got == 0) break;
                used += static_cast<int>(got);
            }
            out[used] = 0;
            ok = (used > 0);
        }
    }
    WinHttpCloseHandle(req);
    return ok;
}

// Picks the page target out of /json/list. match (script "target_url", UTF-8)
// selects the first page whose URL contains it; without a match: the only
// page, or the last one when several are open (new pages append last).
static bool CdpPickTarget(const char* match, wchar_t* wsPath, int wsCap,
                          char* urlOut, int urlCap) {
    char list[32768] = {};
    if (!CdpHttpGet(L"/json/list", list, sizeof(list))) return false;

    struct Page { char url[256]; char ws[256]; };
    Page pages[16];
    int count = 0;

    JsonScan s;
    s.p = list;
    s.end = list + strlen(list);
    JsonWs(&s);
    if (s.p >= s.end || *s.p != '[') return false;
    ++s.p;

    while (s.p < s.end && count < 16) {
        JsonWs(&s);
        if (s.p < s.end && *s.p == ']') break;
        if (s.p >= s.end || *s.p != '{') break;
        ++s.p;

        char url[256] = {}, ws[256] = {}, type[32] = {};
        while (s.p < s.end) {
            JsonWs(&s);
            if (s.p < s.end && *s.p == '}') { ++s.p; break; }
            char key[32] = {};
            if (!JsonString(&s, key, sizeof(key))) return false;
            JsonWs(&s);
            if (s.p >= s.end || *s.p != ':') return false;
            ++s.p;
            JsonWs(&s);
            if (s.p < s.end && *s.p == '"') {
                if (_stricmp(key, "url") == 0) JsonString(&s, url, sizeof(url));
                else if (_stricmp(key, "webSocketDebuggerUrl") == 0) {
                    JsonString(&s, ws, sizeof(ws));
                } else if (_stricmp(key, "type") == 0) {
                    JsonString(&s, type, sizeof(type));
                } else {
                    JsonSkip(&s);
                }
            } else {
                JsonSkip(&s);
            }
            JsonWs(&s);
            if (s.p < s.end && *s.p == ',') ++s.p;
        }
        if (_stricmp(type, "page") == 0 && ws[0] != 0) {
            Page& pg = pages[count++];
            strcpy_s(pg.url, url);
            strcpy_s(pg.ws, ws);
        }
        JsonWs(&s);
        if (s.p < s.end && *s.p == ',') ++s.p;
    }

    int pick = -1;
    if (match != nullptr && match[0] != 0) {
        for (int i = 0; i < count; ++i) {
            if (CdpContainsI(pages[i].url, match)) { pick = i; break; }
        }
    }
    if (pick < 0 && count == 1) pick = 0;
    if (pick < 0 && count > 1) pick = count - 1;
    if (pick < 0) return false;

    // webSocketDebuggerUrl = "ws://127.0.0.1:<port>/devtools/page/<id>" - the
    // request path needs the LEADING slash or the websocket upgrade option
    // rejects the handle with ERROR_INVALID_PARAMETER.
    const char* dp = strstr(pages[pick].ws, "/devtools/page/");
    if (dp == nullptr) return false;
    MultiByteToWideChar(CP_UTF8, 0, dp, -1, wsPath, wsCap);

    if (urlOut != nullptr && urlCap > 0) {
        strncpy_s(urlOut, urlCap, pages[pick].url, _TRUNCATE);
    }
    return true;
}

static bool CdpFailOnce(const char* why) {
    g_cdpFailed = true;
    CdpCloseSocket();
    char buf[192] = {};
    sprintf_s(buf, "script: cdp unavailable (%s) - selector rows fall back to x/y",
              why);
    LogFromOverlay(buf);
    return false;
}

// Connects to the endpoint lazily; idempotent while the socket lives. A dead
// socket (page navigated away) is re-resolved on the next selector row of the
// same run - only the very first failure latches for the whole run.
static bool CdpEnsure(const char* targetMatch) {
    if (g_cdpWs != nullptr) return true;
    if (g_cdpFailed) return false;

    if (g_cdpSession == nullptr) {
        g_cdpSession = WinHttpOpen(L"pplugin/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        if (g_cdpSession == nullptr) return CdpFailOnce("winhttp open");
        // resolve / connect / send / receive
        WinHttpSetTimeouts(g_cdpSession, 3000, 3000, 3000, 4000);
    }
    if (g_cdpConnect == nullptr) {
        g_cdpConnect = WinHttpConnect(g_cdpSession, L"127.0.0.1", kCdpPort, 0);
        if (g_cdpConnect == nullptr) return CdpFailOnce("winhttp connect");
    }

    wchar_t wsPath[128] = {};
    char url[256] = {};
    if (!CdpPickTarget(targetMatch, wsPath, 128, url, sizeof(url))) {
        return CdpFailOnce("no page target on 127.0.0.1:9223");
    }

    HINTERNET req = WinHttpOpenRequest(g_cdpConnect, L"GET", wsPath, nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (req == nullptr) return CdpFailOnce("ws request");
    // The upgrade option takes NO buffer - (NULL, 0); any pointer makes it
    // fail with ERROR_INVALID_PARAMETER (official WinhttpWebsocket sample).
    DWORD err = 0;
    bool ok = (WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                                nullptr, 0) != FALSE);
    if (ok && (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE)) {
        ok = false;
    }
    if (ok && (WinHttpReceiveResponse(req, nullptr) == FALSE)) ok = false;
    HINTERNET ws = nullptr;
    if (ok) ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (ws == nullptr) {
        err = GetLastError();
        WinHttpCloseHandle(req);
        char why[64] = {};
        sprintf_s(why, "websocket upgrade (err=%lu)", err);
        return CdpFailOnce(why);
    }
    WinHttpCloseHandle(req);

    g_cdpWs = ws;
    char buf[320] = {};
    sprintf_s(buf, "script: cdp connect ok target=\"%s\"", url);
    LogFromOverlay(buf);
    return true;
}

static bool CdpSendText(const char* text) {
    if (g_cdpWs == nullptr) return false;
    return WinHttpWebSocketSend(g_cdpWs,
                                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                const_cast<PVOID>(static_cast<const void*>(text)),
                                static_cast<DWORD>(strlen(text))) == NO_ERROR;
}

// Reads one complete text message (fragments joined); false on close/error.
static bool CdpRecvMessage(char* out, int cap) {
    int used = 0;
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type =
            WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
        // Receive returns a DWORD error code (0 = success), not a BOOL.
        if (WinHttpWebSocketReceive(g_cdpWs, out + used, cap - 1 - used,
                                    &read, &type) != NO_ERROR) {
            return false;
        }
        used += static_cast<int>(read);
        out[used] = 0;
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return true;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
        if (used >= cap - 1) return false;
    }
}

// One command round-trip. Responses are matched by their leading {"id":N,
// events and unrelated responses in between are drained. A send/receive
// failure closes the socket so the next CdpEnsure re-resolves the target.
static bool CdpCall(const char* method, const char* params, char* resp, int cap) {
    if (g_cdpWs == nullptr) return false;

    const int id = ++g_cdpMsgId;
    char msg[8192] = {};
    sprintf_s(msg, sizeof(msg), "{\"id\":%d,\"method\":\"%s\",\"params\":%s}",
              id, method, (params != nullptr && params[0] != 0) ? params : "{}");
    if (!CdpSendText(msg)) {
        CdpCloseSocket();
        return false;
    }

    char prefix[32] = {};
    sprintf_s(prefix, sizeof(prefix), "{\"id\":%d,", id);

    char buf[8192] = {};
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (!CdpRecvMessage(buf, sizeof(buf))) {
            CdpCloseSocket();
            return false;
        }
        if (strncmp(buf, prefix, strlen(prefix)) != 0) continue;   // event, skip
        if (resp != nullptr && cap > 0) {
            strncpy_s(resp, cap, buf, _TRUNCATE);
        }
        return true;
    }
    return false;
}

// Evaluates expr in the page and returns its string result in out.
static bool CdpEvaluate(const char* expr, char* out, int cap) {
    char quoted[2048] = {};
    CdpJsonQuote(expr, quoted, sizeof(quoted));
    char params[2200] = {};
    sprintf_s(params, sizeof(params), "{\"expression\":%s}", quoted);

    char resp[8192] = {};
    if (!CdpCall("Runtime.evaluate", params, resp, sizeof(resp))) return false;
    if (strstr(resp, "\"exceptionDetails\"") != nullptr) return false;

    const char* v = strstr(resp, "\"value\":\"");
    if (v == nullptr) return false;
    JsonScan s;
    s.p = v + 8;   // on the opening quote of the value
    s.end = resp + strlen(resp);
    return JsonString(&s, out, cap);
}

struct CdpRect {
    float x = 0, y = 0, w = 0, h = 0, vw = 0, vh = 0;
    int   found = 0;
};

// Resolves a CSS selector to the element's center in CSS px (viewport
// coordinates - the same space Input.dispatchMouseEvent takes). The element
// is scrolled to with block:'nearest' first, which is a no-op when visible.
// Returns false on channel/JS errors; found=0 means "no such element".
static bool CdpQuerySelector(const char* selector, CdpRect* out) {
    char selQ[512] = {};
    CdpJsonQuote(selector, selQ, sizeof(selQ));
    // A JSON string literal is a valid JS string literal (same quotes, same
    // escapes), so the quoted selector embeds directly - no JSON.parse needed.
    char expr[1024] = {};
    sprintf_s(expr, sizeof(expr),
        "(function(){var e=document.querySelector(%s);"
        "if(!e)return '{\"found\":0}';"
        "e.scrollIntoView({block:'nearest',inline:'nearest'});"
        "var r=e.getBoundingClientRect();"
        "return JSON.stringify({found:1,x:r.left+r.width/2,y:r.top+r.height/2,"
        "w:r.width,h:r.height,vw:innerWidth,vh:innerHeight});})()",
        selQ);

    char val[256] = {};
    if (!CdpEvaluate(expr, val, sizeof(val))) return false;

    *out = CdpRect();
    JsonScan s;
    s.p = val;
    s.end = val + strlen(val);
    JsonWs(&s);
    if (s.p >= s.end || *s.p != '{') return false;
    ++s.p;
    while (s.p < s.end) {
        JsonWs(&s);
        if (s.p < s.end && *s.p == '}') break;
        char key[16] = {};
        if (!JsonString(&s, key, sizeof(key))) return false;
        JsonWs(&s);
        if (s.p >= s.end || *s.p != ':') return false;
        ++s.p;
        JsonWs(&s);
        double v = 0;
        if (JsonNumber(&s, &v)) {
            if (strcmp(key, "found") == 0) out->found = static_cast<int>(v);
            else if (strcmp(key, "x") == 0) out->x = static_cast<float>(v);
            else if (strcmp(key, "y") == 0) out->y = static_cast<float>(v);
            else if (strcmp(key, "w") == 0) out->w = static_cast<float>(v);
            else if (strcmp(key, "h") == 0) out->h = static_cast<float>(v);
        } else {
            JsonSkip(&s);
        }
        JsonWs(&s);
        if (s.p < s.end && *s.p == ',') ++s.p;
    }
    return true;
}

static bool CdpMouse(const char* type, float x, float y,
                     const char* button, int clickCount) {
    char params[192] = {};
    if (button != nullptr && button[0] != 0) {
        sprintf_s(params, sizeof(params),
                  "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"button\":\"%s\","
                  "\"clickCount\":%d}",
                  type, x, y, button, clickCount);
    } else {
        sprintf_s(params, sizeof(params),
                  "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f}", type, x, y);
    }
    return CdpCall("Input.dispatchMouseEvent", params, nullptr, 0);
}

// The visible part of a selector for log lines (long selectors get trimmed).
static void CdpSelBrief(const char* selector, char* out, int cap) {
    strncpy_s(out, cap, selector[0] != 0 ? selector : "?", 40);
}

static bool CdpClickAt(float x, float y, int kind, const char* selector) {
    if (!CdpMouse("mouseMoved", x, y, nullptr, 0)) return false;
    Sleep(20);   // let the hover state land, like the SendInput path does

    const char* btn = (kind == 2) ? "right" : "left";
    if (!CdpMouse("mousePressed", x, y, btn, 1)) return false;
    if (!CdpMouse("mouseReleased", x, y, btn, 1)) return false;
    if (kind == 1) {  // double: the second pair carries clickCount 2
        Sleep(40);    // same gap the SendInput double uses
        if (!CdpMouse("mousePressed", x, y, btn, 2)) return false;
        if (!CdpMouse("mouseReleased", x, y, btn, 2)) return false;
    }

    char sel[48] = {};
    CdpSelBrief(selector, sel, sizeof(sel));
    char buf[160] = {};
    sprintf_s(buf, "script: cdp click kind=%d \"%s\" %.0f,%.0f (page px, trusted input)",
              kind, sel, x, y);
    LogFromOverlay(buf);
    return true;
}

// Wheel over the selector's element. CDP deltaY is pixels, positive scrolls
// down; the JSON wheel keeps Windows semantics (notches, positive = up), so
// one notch maps to -100 px.
static bool CdpWheelSelector(const ScriptEvent& ev) {
    CdpRect rc;
    if (!CdpQuerySelector(ev.selector, &rc) || rc.found == 0 ||
        rc.w < 0.5f || rc.h < 0.5f) {
        return false;
    }
    char params[160] = {};
    sprintf_s(params, sizeof(params),
              "{\"type\":\"mouseWheel\",\"x\":%.1f,\"y\":%.1f,\"deltaX\":0,"
              "\"deltaY\":%d}",
              rc.x, rc.y, -100 * ev.wheel);
    if (!CdpCall("Input.dispatchMouseEvent", params, nullptr, 0)) return false;

    char sel[48] = {};
    CdpSelBrief(ev.selector, sel, sizeof(sel));
    char buf[160] = {};
    sprintf_s(buf, "script: cdp wheel %+d \"%s\" %.0f,%.0f (page px)",
              ev.wheel, sel, rc.x, rc.y);
    LogFromOverlay(buf);
    return true;
}

// True when the click was delivered over CDP. found=0, a hidden element and
// every channel error return false so the caller can fall back to x/y.
static bool CdpClickSelector(const ScriptEvent& ev) {
    CdpRect rc;
    if (!CdpQuerySelector(ev.selector, &rc)) {
        char sel[48] = {};
        CdpSelBrief(ev.selector, sel, sizeof(sel));
        char buf[160] = {};
        sprintf_s(buf, "script: cdp \"%s\" evaluate failed", sel);
        LogFromOverlay(buf);
        return false;
    }
    if (rc.found == 0 || rc.w < 0.5f || rc.h < 0.5f) return false;
    return CdpClickAt(rc.x, rc.y, ev.click, ev.selector);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// Two backends for coordinate rows:
//
//   msg  (default) - PostMessage(WM_*BUTTONDOWN/UP, WM_MOUSEWHEEL) straight at
//         the view window. No cursor movement, no foreground requirement, no
//         occlusion check. Reaches Flutter's own widgets, because they consume
//         ordinary window messages. WebView2 content (the H5 popups composited
//         into Flutter) does NOT react to posted messages - that is exactly what
//         the CDP selector path above exists for (verified 2026-10).
//   send  - SendInput, hardware-grade input with the real cursor. It is
//         indistinguishable from a physical mouse and does reach WebView2, but
//         it needs the app in front and the point unobstructed.
//
// Switch with "click_input=send" in <pluginDir>\config.ini.
static bool g_inputMsg = true;

static void SendMouse(ULONG flags, SHORT wheelDelta) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = static_cast<DWORD>(wheelDelta);
    SendInput(1, &in, sizeof(INPUT));
}

// SendInput backend: real cursor, raises the window, skips when covered.
static void ClickSendAt(HWND view, LONG px, LONG py, int kind) {
    POINT pt = {px, py};
    if (!ClientToScreen(view, &pt)) return;

    if (g_hwnd) SetForegroundWindow(g_hwnd);   // so the point is clickable
    Sleep(200);

    HWND hit = WindowFromPoint(pt);
    if (hit != view) {
        wchar_t wcls[64] = {};
        char cover[64] = {};
        GetClassNameW(hit ? hit : g_hwnd, wcls, 64);
        Nz(wcls, cover, sizeof(cover));
        char buf[192] = {};
        sprintf_s(buf, "script: click skipped, (%ld,%ld) covered by %s - "
                       "bring the TTVoice window to front",
                  pt.x, pt.y, cover);
        LogFromOverlay(buf);
        return;
    }

    if (!SetCursorPos(pt.x, pt.y)) return;
    Sleep(50);   // let the hover frame land before the button press

    const bool right = (kind == 2);
    const ULONG down = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    const ULONG up = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendMouse(down, 0);
    SendMouse(up, 0);
    if (kind == 1) {  // double
        Sleep(40);
        SendMouse(down, 0);
        SendMouse(up, 0);
    }

    char buf[128] = {};
    sprintf_s(buf, "script: click kind=%d %ld,%ld (real input, screen %ld,%ld)",
              kind, px, py, pt.x, pt.y);
    LogFromOverlay(buf);
}

// Message backend: posted to the view window, client coordinates, no cursor.
static void ClickMsgAt(HWND view, LONG px, LONG py, int kind) {
    const LPARAM pos = MAKELPARAM(px, py);

    PostMessageW(view, WM_MOUSEMOVE, 0, pos);
    Sleep(10);   // lets the hover frame land before the press

    if (kind == 2) {   // right
        PostMessageW(view, WM_RBUTTONDOWN, MK_RBUTTON, pos);
        Sleep(15);
        PostMessageW(view, WM_RBUTTONUP, 0, pos);
    } else if (kind == 1) {   // double
        PostMessageW(view, WM_LBUTTONDOWN, MK_LBUTTON, pos);
        Sleep(15);
        PostMessageW(view, WM_LBUTTONUP, 0, pos);
        Sleep(40);   // same gap the SendInput double uses
        PostMessageW(view, WM_LBUTTONDBLCLK, MK_LBUTTON, pos);
        Sleep(15);
        PostMessageW(view, WM_LBUTTONUP, 0, pos);
    } else {   // left
        PostMessageW(view, WM_LBUTTONDOWN, MK_LBUTTON, pos);
        Sleep(15);
        PostMessageW(view, WM_LBUTTONUP, 0, pos);
    }

    char buf[128] = {};
    sprintf_s(buf, "script: click kind=%d %ld,%ld (posted message)",
              kind, px, py);
    LogFromOverlay(buf);
}

static void SimulateClickKindAt(float lx, float ly, int kind) {
    HWND view = g_viewHwnd ? g_viewHwnd : g_hwnd;
    if (!view) return;

    const LONG px = static_cast<LONG>(lx * g_scale);
    const LONG py = static_cast<LONG>(ly * g_scale);
    if (g_inputMsg) {
        ClickMsgAt(view, px, py, kind);
    } else {
        ClickSendAt(view, px, py, kind);
    }
}

// SendInput backend for the wheel: real cursor, same occlusion rules.
static void WheelSendAt(HWND view, LONG px, LONG py, int notches) {
    POINT pt = {px, py};
    if (!ClientToScreen(view, &pt)) return;

    if (g_hwnd) SetForegroundWindow(g_hwnd);
    Sleep(200);

    HWND hit = WindowFromPoint(pt);
    if (hit != view) {
        LogFromOverlay("script: wheel skipped, point covered by another window");
        return;
    }

    if (!SetCursorPos(pt.x, pt.y)) return;
    Sleep(50);
    SendMouse(MOUSEEVENTF_WHEEL, static_cast<SHORT>(120 * notches));

    char buf[128] = {};
    sprintf_s(buf, "script: wheel %+d at %ld,%ld (real input, screen %ld,%ld)",
              notches, px, py, pt.x, pt.y);
    LogFromOverlay(buf);
}

// Message backend for the wheel: WM_MOUSEWHEEL carries SCREEN coordinates.
static void WheelMsgAt(HWND view, LONG px, LONG py, int notches) {
    POINT pt = {px, py};
    if (!ClientToScreen(view, &pt)) return;

    const WPARAM wp = MAKEWPARAM(0, static_cast<WORD>(120 * notches));
    PostMessageW(view, WM_MOUSEWHEEL, wp, MAKELPARAM(pt.x, pt.y));

    char buf[128] = {};
    sprintf_s(buf, "script: wheel %+d at %ld,%ld (posted message)",
              notches, px, py);
    LogFromOverlay(buf);
}

static void SimulateWheelAt(float lx, float ly, int notches) {
    if (!notches) return;
    HWND view = g_viewHwnd ? g_viewHwnd : g_hwnd;
    if (!view) return;

    const LONG px = static_cast<LONG>(lx * g_scale);
    const LONG py = static_cast<LONG>(ly * g_scale);
    if (g_inputMsg) {
        WheelMsgAt(view, px, py, notches);
    } else {
        WheelSendAt(view, px, py, notches);
    }
}

// Puts the text on the clipboard and sends Ctrl+V to the app. Only lands when
// pplugin owns the foreground window, so it is brought up first.
static void PasteTextToApp(const wchar_t* text) {
    if (!text || !text[0]) return;

    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    if (!OpenClipboard(nullptr)) {
        LogFromOverlay("script: clipboard busy, paste skipped");
        return;
    }
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, text, bytes);
            GlobalUnlock(mem);
        }
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();

    if (g_hwnd) SetForegroundWindow(g_hwnd);
    Sleep(80);

    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = 'V';
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, static_cast<int>(sizeof(INPUT)));

    char narrow[512] = {};
    char buf[560] = {};
    sprintf_s(buf, "script: pasted \"%s\"", Nz(text, narrow, sizeof(narrow)));
    LogFromOverlay(buf);
}

// One event's actions in engine order (wheel -> paste -> click). A selector
// event resolves its element over CDP and clicks it there - trusted input,
// no foreground needed. Every failure path (endpoint down, element missing,
// dispatch error) falls back to the recorded x/y SendInput click, which is
// exactly what the same row did before selectors existed.
static void RunEventActions(const ScriptEvent& ev, const char* targetUrl) {
    const bool wantCdp = (ev.selector[0] != 0);
    const bool cdp = wantCdp && CdpEnsure(targetUrl);

    if (ev.wheel != 0) {
        if (!(cdp && CdpWheelSelector(ev))) {
            SimulateWheelAt(ev.x, ev.y, ev.wheel);
        }
    }
    if (ev.text[0]) {
        PasteTextToApp(ev.text);
    }

    if (!wantCdp) {
        SimulateClickKindAt(ev.x, ev.y, ev.click);
        return;
    }
    if (cdp && CdpClickSelector(ev)) {
        return;
    }

    // No element (or no channel): the recorded point is the fallback. x/y 0,0
    // means "never picked" - skip instead of clicking the top-left corner.
    if (ev.x == 0.0f && ev.y == 0.0f) {
        char sel[48] = {};
        CdpSelBrief(ev.selector, sel, sizeof(sel));
        char buf[160] = {};
        sprintf_s(buf, "script: cdp \"%s\" not found and no x/y fallback, skipped",
                  sel);
        LogFromOverlay(buf);
        return;
    }
    if (cdp) {
        char sel[48] = {};
        CdpSelBrief(ev.selector, sel, sizeof(sel));
        char buf[160] = {};
        sprintf_s(buf, "script: cdp \"%s\" missing, click falls back to x/y", sel);
        LogFromOverlay(buf);
    }
    SimulateClickKindAt(ev.x, ev.y, ev.click);
}

static DWORD WINAPI ScriptRunThread(LPVOID) {
    Script local;  // detached copy, so a reload cannot tear the run apart
    AcquireSRWLockShared(&g_scriptLock);
    local = g_script;
    ReleaseSRWLockShared(&g_scriptLock);

    {
        char nm[256] = {};
        char buf[320] = {};
        sprintf_s(buf, "script: run \"%s\" events=%d loop=%d",
                  Nz(local.name, nm, sizeof(nm)), local.count, local.loop);
        LogFromOverlay(buf);
    }

    CdpRunBegin();
    const ULONGLONG started = GetTickCount64();

    for (int round = 1; round <= local.loop; ++round) {
        for (int i = 0; i < local.count; ++i) {
            const ScriptEvent& ev = local.events[i];
            RunEventActions(ev, local.targetUrl);

            if (round == local.loop && i == local.count - 1) continue;
            if (ev.delayMs > 0) Sleep(static_cast<DWORD>(ev.delayMs));
        }
    }

    char buf[160] = {};
    sprintf_s(buf, "script: done in %llums", static_cast<unsigned long long>(
                  GetTickCount64() - started));
    LogFromOverlay(buf);

    InterlockedExchange(&g_scriptActive, 0);
    return 0;
}

static bool ScriptRun() {
    if (g_script.count <= 0) return false;
    if (InterlockedCompareExchange(&g_scriptActive, 1, 0) != 0) {
        LogFromOverlay("script: run ignored, previous run still active");
        return true;
    }

    HANDLE th = CreateThread(nullptr, 0, &ScriptRunThread, nullptr, 0, nullptr);
    if (!th) {
        InterlockedExchange(&g_scriptActive, 0);
        LogFromOverlay("script: CreateThread failed");
        return true;
    }
    CloseHandle(th);
    InterlockedIncrement(&g_scriptRuns);
    return true;
}

// Called from the overlay button (window thread). Returns true when a script
// exists and took the click over; false means there is nothing to run. verbose
// makes ScriptReload print the reason even when nothing about the file changed.
static bool ScriptTryRun(bool verbose) {
    if (!ScriptReload(verbose)) return false;
    return ScriptRun();
}

static DWORD WINAPI ScriptHotkeyThread(LPVOID) {
    for (;;) {
        ScriptReload(false);            // background poll: stay quiet on repeat
        // Hotkey ownership moved to the framework dispatcher (Host::addHotkey),
        // so this thread only keeps the pill / status line fresh. Drain any
        // stray messages so MsgWaitForMultipleObjects does not spin on them.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 2000, QS_ALLINPUT);
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            // nothing to do
        }
    }
}

static bool g_hotkeyThreadStarted = false;

static void ScriptStartHotkey() {
    if (g_hotkeyThreadStarted) return;
    g_hotkeyThreadStarted = true;
    HANDLE th = CreateThread(nullptr, 0, &ScriptHotkeyThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}
