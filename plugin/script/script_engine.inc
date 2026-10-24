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
//    "events":[{"x":27,"y":113,"click":"left","delay_ms":600,"wheel":0,
//               "text":"","wait_px":0,"wait_max_ms":0}, ...]}
// Coordinates are logical (96 dpi) view-relative, exactly like overlay.ini.
//
// Keep this file pure ASCII.

static const int kScriptMaxEvents = 32;
static const int kScriptText      = 256;
static const int kScriptHotkeyId  = 0x5454;  // "TT"

struct ScriptEvent {
    float   x = 0.0f;
    float   y = 0.0f;
    int     click = 0;       // 0 = left, 1 = double, 2 = right
    int     delayMs = 0;     // wait after this row
    int     wheel = 0;       // notches, negative scrolls down
    int     waitPx = 0;      // gate: cream pixels required around (x,y)
    int     waitMaxMs = 0;   // gate limit; 0 = no gate
    wchar_t text[kScriptText] = {};
};

struct Script {
    wchar_t     name[64] = {};
    int         loop = 1;
    int         hotkeyVk = 0;
    int         count = 0;
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
        } else if (_stricmp(key, "wait_px") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->waitPx = static_cast<int>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "wait_max_ms") == 0) {
            double v = 0;
            if (JsonNumber(s, &v)) ev->waitMaxMs = static_cast<int>(v);
            else JsonSkip(s);
        } else if (_stricmp(key, "text") == 0) {
            char text[1024] = {};
            if (JsonString(s, text, sizeof(text))) Utf8ToWideN(text, ev->text, kScriptText);
            else JsonSkip(s);
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
        } else {
            JsonSkip(&s);
        }

        JsonWs(&s);
        if (s.p < s.end && *s.p == ',') ++s.p;
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
// Actions
// ---------------------------------------------------------------------------

static void SimulateClickKindAt(float lx, float ly, int kind) {
    HWND target = g_viewHwnd ? g_viewHwnd : g_hwnd;
    if (!target) return;

    const LONG px = static_cast<LONG>(lx * g_scale);
    const LONG py = static_cast<LONG>(ly * g_scale);
    const LPARAM lp = MAKELPARAM(px, py);

    PostMessageW(target, WM_MOUSEMOVE, 0, lp);
    if (kind == 2) {  // right
        PostMessageW(target, WM_RBUTTONDOWN, MK_RBUTTON, lp);
        PostMessageW(target, WM_RBUTTONUP, 0, lp);
    } else if (kind == 1) {  // double
        PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageW(target, WM_LBUTTONUP, 0, lp);
        Sleep(40);
        PostMessageW(target, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
        PostMessageW(target, WM_LBUTTONUP, 0, lp);
    } else {
        PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageW(target, WM_LBUTTONUP, 0, lp);
    }

    char buf[128] = {};
    sprintf_s(buf, "script: click kind=%d %ld,%ld", kind, px, py);
    LogFromOverlay(buf);
}

static void SimulateWheelAt(float lx, float ly, int notches) {
    HWND target = g_viewHwnd ? g_viewHwnd : g_hwnd;
    if (!target || notches == 0) return;

    // WM_MOUSEWHEEL takes screen coordinates, unlike the click messages.
    POINT pt = {static_cast<LONG>(lx * g_scale), static_cast<LONG>(ly * g_scale)};
    if (!ClientToScreen(target, &pt)) return;

    const int delta = 120 * notches;
    const WPARAM wp =
        static_cast<WPARAM>(static_cast<WORD>(delta & 0xFFFF)) << 16;
    PostMessageW(target, WM_MOUSEWHEEL, wp, MAKELPARAM(pt.x, pt.y));

    char buf[128] = {};
    sprintf_s(buf, "script: wheel %+d at %ld,%ld", notches, pt.x, pt.y);
    LogFromOverlay(buf);
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

// Waits (up to maxMs) until at least minPx cream pixels sit around (lx,ly) for
// three polls in a row. Returns the elapsed ms, or -1 on timeout.
static long WaitForCreamAt(float lx, float ly, int minPx, int maxMs, int pollMs) {
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG limit = (maxMs > 0) ? static_cast<ULONGLONG>(maxMs) : 4000ULL;
    int stable = 0;
    int maxHits = 0;

    while (GetTickCount64() - start < limit) {
        Sleep(static_cast<DWORD>(pollMs));
        if (!CaptureView()) continue;

        LONG bx = 0;
        LONG by = 0;
        const int n = CountCreamBand(lx, ly, &bx, &by);
        if (n > maxHits) maxHits = n;
        if (n >= minPx) {
            if (++stable >= 3) {
                char buf[160] = {};
                sprintf_s(buf, "script: gate hit %dpx at %ld,%ld after %llums", n, bx, by,
                          static_cast<unsigned long long>(GetTickCount64() - start));
                LogFromOverlay(buf);
                return static_cast<long>(GetTickCount64() - start);
            }
        } else {
            stable = 0;
        }
    }

    char buf[160] = {};
    sprintf_s(buf, "script: gate timeout %lums (max %dpx)", static_cast<unsigned long>(limit),
              maxHits);
    LogFromOverlay(buf);
    return -1;
}

static DWORD WINAPI ScriptRunThread(LPVOID) {
    Script local;  // detached copy, so a reload cannot tear the run apart
    AcquireSRWLockShared(&g_scriptLock);
    local = g_script;
    ReleaseSRWLockShared(&g_scriptLock);
    const int poll = (g_gatePollMs >= 30) ? g_gatePollMs : 120;

    {
        char nm[256] = {};
        char buf[320] = {};
        sprintf_s(buf, "script: run \"%s\" events=%d loop=%d",
                  Nz(local.name, nm, sizeof(nm)), local.count, local.loop);
        LogFromOverlay(buf);
    }

    const ULONGLONG started = GetTickCount64();

    for (int round = 1; round <= local.loop; ++round) {
        for (int i = 0; i < local.count; ++i) {
            const ScriptEvent& ev = local.events[i];

            if (ev.waitPx > 0) {
                WaitForCreamAt(ev.x, ev.y, ev.waitPx, ev.waitMaxMs, poll);
            }
            if (ev.wheel != 0) {
                SimulateWheelAt(ev.x, ev.y, ev.wheel);
            }
            if (ev.text[0]) {
                PasteTextToApp(ev.text);
            }
            SimulateClickKindAt(ev.x, ev.y, ev.click);

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
