#include "lyrics.h"

#include "game.h"
#include "util.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

// The game's music plays one .ogg per song (music/playlist_*.bed), and the mod shows its lyrics, karaoke
// style, at the bottom of the headset's picture. The song's start and its path are taken from the game's
// own playlist advance (kMusicPlayCall: one call site per playlist), the song's position is kept by the
// mod's own clock that runs while the game's music plays (its state tells when: see game.h), so pauses
// hold it. The lyrics themselves come from the player: lyrics\*.lrc next to the mod (the standard
// [mm:ss.xx] format, the extended <mm:ss.xx> word timings when the file has them), matched to a song by
// its [ti:] title or [ar:] artist when they fit the game's own, else by the file's name (01.ogg -> 01.lrc,
// or any .lrc whose name begins with it: the game's own files are data/songs1/01.ogg and the like).
// The mod ships no lyrics: the songs are the artists' own work.

namespace lyrics {

namespace {

// ---- the hook into the game's music ---------------------------------------------------------------

uintptr_t oMusicPlay = fo2::kMusicPlay;
std::string g_path;           // the playing .ogg's path, as the game named it (forward slashes, lowercase)
double g_startPos;            // the song's start, seconds into the file
bool g_playing = false;       // a song was started (its position is kept below)
double g_position;            // the song's position, seconds into the file
bool g_racing = false;        // the playing song's playlist is one that races play (2 stunts, 3 races)

// The .lrc files: read once at the first song, each with the lines the file tells.
struct Song
{
    std::wstring name;          // the .lrc's own name (01.lrc), forward slashes, lowercase
    std::string path;           // the game's .ogg path it wants (from [file:], when the file tells it)
    std::string title, artist;  // [ti:] and [ar:], lowercase, as the file tells them
    std::vector<Line> lines;    // by their time, the last line's until filled from the next
    bool hasWords = false;      // some line tells its words' times
};
std::vector<Song> g_songs;
bool g_songsRead = false;
int g_song = -1;               // the playing song's lyrics (into g_songs), -1 none
double g_songAt = -1e9;        // when its lyrics were picked (the song may repeat)
bool g_bannerPending = false;  // check kMusicBanner on the next Tick (filled right after MusicPlay returns)
const void* g_pendingData = nullptr;
int g_pendingPlaylist = 0;

// The word timings of the line being shown: drawn as the words are sung (see Draw), for the files that
// tell them; when only the line's time is known, the whole line turns bright at once.
constexpr double kFade = 0.35;  // seconds a line takes to fade in and out

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

// "data/songs1/01.ogg" -> "01", with the folder kept when it tells one song from another
std::wstring SongKey(const std::string& path)
{
    std::wstring p;
    for (char c : path) p.push_back((unsigned char)c < 0x80 ? (wchar_t)towlower((unsigned char)c) : L'?');
    size_t slash = p.find_last_of(L"/\\");
    if (slash != std::wstring::npos) p = p.substr(slash + 1);
    size_t dot = p.find_last_of(L'.');
    if (dot != std::wstring::npos) p.resize(dot);
    return p;
}

// ---- .lrc files -----------------------------------------------------------------------------------

// A UTF-8 or UTF-16 (with a BOM) text file as UTF-16; an empty string when it cannot be read.
std::wstring ReadText(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    char raw[65536];
    DWORD n = 0;
    BOOL ok = ReadFile(f, raw, sizeof(raw), &n, nullptr);
    CloseHandle(f);
    if (!ok || n < 2) return n > 0 ? std::wstring(raw, raw + n) : L"";
    std::wstring out;
    if ((BYTE)raw[0] == 0xFF && (BYTE)raw[1] == 0xFE)  // UTF-16 LE
        out.assign((const wchar_t*)(raw + 2), (const wchar_t*)(raw + n));
    else if ((BYTE)raw[0] == 0xFE && (BYTE)raw[1] == 0xFF)  // UTF-16 BE: swapped
    {
        for (DWORD i = 2; i + 1 < n; i += 2)
        {
            wchar_t c = (BYTE)raw[i + 1];
            out.push_back(c | ((wchar_t)(BYTE)raw[i] << 8));
        }
    }
    else
    {
        bool bom = n >= 3 && (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF;
        const char* s = raw + (bom ? 3 : 0);
        DWORD len = n - (bom ? 3 : 0);
        UINT cp = CP_UTF8;
        int need = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s, len, nullptr, 0);
        if (need <= 0)
        {
            cp = 1252;
            need = MultiByteToWideChar(cp, 0, s, len, nullptr, 0);
        }
        out.resize((size_t)std::max(need, 0));
        if (need > 0) MultiByteToWideChar(cp, 0, s, len, out.data(), need);
    }
    return out;
}

// "mm:ss.xx" or "[mm:ss.xx]" (the .xx may be .x, .xxx or nothing); false when it is not one.
bool TimeTag(const wchar_t* s, double* out)
{
    if (!s) return false;
    if (*s == L'[') s++;
    wchar_t* end = nullptr;
    double whole = wcstod(s, &end);
    if (end == s || *end != L':') return false;
    s = end + 1;
    double part = wcstod(s, &end);
    if (end == s) return false;
    if (*end == L']') end++;
    if (*end != L'\0') return false;
    if (whole < 0 || part < 0 || part >= 60) return false;
    *out = whole * 60 + part;
    return true;
}

void AddLine(Song& song, double at, std::wstring text)
{
    Line line;
    line.at = at;
    // The extended .lrc tells each word's time ("<mm:ss.xx>word word..."): taken out of the text, kept
    // with their times so the words light up as they are sung; the plain text draws the same.
    std::wstring plain;
    const wchar_t* q = text.c_str();
    int w = 0;
    double wordAt = at;
    while (*q)
    {
        if (q[0] == L'<' && iswdigit(q[1]))
        {
            const wchar_t* t = q + 1;
            wchar_t* end = nullptr;
            double whole = wcstod(t, &end);
            if (end != t && *end == L':')
            {
                double part = wcstod(end + 1, &end);
                if (*end == L'>')
                {
                    wordAt = whole * 60 + part;
                    q = end + 1;
                    const wchar_t* stop = q;  // the word's text: to the next word's time
                    while (*stop && *stop != L'<') stop++;
                    std::wstring word(q, stop);
                    if (!word.empty() && w < 64)
                    {
                        line.words[w].text = word;
                        line.words[w].at = wordAt;
                        w++;
                        plain += word;
                    }
                    q = stop;
                    continue;
                }
            }
        }
        plain.push_back(*q++);
    }
    if (w > 0) song.hasWords = true;
    while (!plain.empty() && iswspace(plain.back())) plain.pop_back();
    if (plain.empty() && !w) return;
    line.plain = plain;
    line.wordCount = w;
    song.lines.push_back(line);
}

void ReadLrc(const std::wstring& path)
{
    std::wstring text = ReadText(path);
    if (text.empty()) return;
    Song song;
    song.name = Lower(path.substr(path.find_last_of(L"/\\") + 1));
    std::vector<std::pair<double, std::wstring>> timed;
    double offset = 0;
    bool any = false;
    wchar_t* s = &text[0];
    while (s && *s)
    {
        while (*s && *s != L'[') s++;
        if (!*s) break;
        wchar_t* tag = s + 1;
        wchar_t* close = wcschr(tag, L']');
        if (!close) break;
        *close = L'\0';
        wchar_t* line = close + 1;
        wchar_t* end = wcspbrk(line, L"\r\n");
        std::wstring value;
        if (end)
        {
            value.assign(line, end - line);
            *end = L'\0';
            s = end + 1;
        }
        else
        {
            value.assign(line);
            s = line + wcslen(line);
        }
        // A time tag names when the line after it is sung; a named tag tells about the whole file.
        double at = 0;
        if (TimeTag(tag, &at))
        {
            timed.emplace_back(at, value);
            any = true;
            continue;
        }
        wchar_t* colon = wcschr(tag, L':');
        if (!colon) continue;
        *colon = L'\0';
        std::wstring key = Lower(tag);
        const wchar_t* val = colon + 1;
        while (*val && iswspace(*val)) val++;
        std::string v;
        for (const wchar_t* p = val; *p; p++) v.push_back((unsigned char)*p < 0x80 ? (char)*p : '?');
        while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
        if (key == L"ti") song.title = Lower(v);
        else if (key == L"ar") song.artist = Lower(v);
        else if (key == L"file") song.path = Lower(v);
        else if (key == L"offset")
        {
            wchar_t* e = nullptr;
            double off = wcstod(val, &e);
            if (e != val) offset = off / 1000.0;  // [offset:] is in milliseconds
        }
    }
    if (!any) return;
    std::stable_sort(timed.begin(), timed.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [at, value] : timed)
    {
        std::wstring v = value;
        while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
        AddLine(song, at + offset, v);
    }
    for (size_t i = 0; i + 1 < song.lines.size(); i++) song.lines[i].until = song.lines[i + 1].at;
    if (!song.lines.empty()) song.lines.back().until = song.lines.back().at + 8;  // stays a while after the last line
    g_songs.push_back(std::move(song));
}

void ReadSongs()
{
    g_songs.clear();
    g_songsRead = true;
    std::wstring dir = ModuleDir() + L"lyrics";
    std::wstring all = dir + L"\\*.lrc";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(all.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ReadLrc(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    Log("lyrics: %d song%s in lyrics\\", (int)g_songs.size(), g_songs.size() == 1 ? "" : "s");
}

// The song whose lyrics fit the playing .ogg: the one whose [file:] is its path, else whose name is the
// .ogg's own (with the game's songs1 folder kept out).
int FindSongByPath(const std::string& path)
{
    std::string p = Lower(path);
    std::wstring key = SongKey(path);
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].path.empty() && g_songs[i].path == p) return (int)i;
    for (size_t i = 0; i < g_songs.size(); i++)
    {
        std::wstring base = g_songs[i].name;
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) base.resize(dot);  // 01.lrc -> 01
        if (base == key) return (int)i;
    }
    return -1;
}

// Reads artist and song title from the playlist vector that holds this in-memory OGG stream.
bool GetSongFromPlaylist(int playlistId, const void* data, std::string& outArtist, std::string& outTitle)
{
    if (!data) return false;

    auto readStdString = [](uintptr_t strAddr) -> std::string {
        if (!strAddr) return "";
        uint32_t cap = *(const uint32_t*)(strAddr + 0x18);
        uint32_t size = *(const uint32_t*)(strAddr + 0x14);
        if (size == 0 || size > 1024) return "";
        const char* s = (cap >= 16) ? *(const char**)(strAddr + 4) : (const char*)(strAddr + 4);
        if (!s) return "";
        return std::string(s, size);
    };

    struct VecDef { int id; uintptr_t beginAddr; uintptr_t endAddr; };
    const VecDef vecs[3] = {
        { 1, 0x8DA844, 0x8DA848 }, // Menus (8 tracks)
        { 2, 0x8DA838, 0x8DA83C }, // Races (19 tracks)
        { 3, 0x8DA82C, 0x8DA830 }  // Stunts (9 tracks)
    };

    // 1. Search matching playlist vector first
    for (const auto& v : vecs)
    {
        if (v.id != playlistId && playlistId > 0) continue;
        uintptr_t begin = *(const uintptr_t*)v.beginAddr;
        uintptr_t end = *(const uintptr_t*)v.endAddr;
        if (begin && end && begin < end)
        {
            for (uintptr_t entry = begin; entry < end; entry += 96)
            {
                if (*(const void**)(entry + 0x58) == data)
                {
                    outArtist = readStdString(entry + 0x1c);
                    outTitle = readStdString(entry + 0x38);
                    if (!outTitle.empty()) return true;
                }
            }
        }
    }
    // 2. If not found in playlistId, search all playlist vectors
    for (const auto& v : vecs)
    {
        if (v.id == playlistId && playlistId > 0) continue;
        uintptr_t begin = *(const uintptr_t*)v.beginAddr;
        uintptr_t end = *(const uintptr_t*)v.endAddr;
        if (begin && end && begin < end)
        {
            for (uintptr_t entry = begin; entry < end; entry += 96)
            {
                if (*(const void**)(entry + 0x58) == data)
                {
                    outArtist = readStdString(entry + 0x1c);
                    outTitle = readStdString(entry + 0x38);
                    if (!outTitle.empty()) return true;
                }
            }
        }
    }

    return false;
}

int FindSong(const std::string& artist, const std::string& song)
{
    if (song.empty()) return -1;
    std::string a = Lower(artist), t = Lower(song);
    // 1. Exact match on title and artist
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].title.empty() && g_songs[i].title == t && (g_songs[i].artist.empty() || a.empty() || g_songs[i].artist == a))
            return (int)i;
    // 2. Exact match on title alone
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].title.empty() && g_songs[i].title == t)
            return (int)i;
    // 3. Substring match (preferring artist match if available)
    if (!a.empty())
    {
        for (size_t i = 0; i < g_songs.size(); i++)
            if (!g_songs[i].title.empty() && g_songs[i].artist == a &&
                (g_songs[i].title.find(t) != std::string::npos || t.find(g_songs[i].title) != std::string::npos))
                return (int)i;
    }
    for (size_t i = 0; i < g_songs.size(); i++)
        if (!g_songs[i].title.empty() && (g_songs[i].title.find(t) != std::string::npos || t.find(g_songs[i].title) != std::string::npos))
            return (int)i;
    return -1;
}

// By the game's own banner (artist and title), when the playlist tells them (filled right after MusicPlay returns).
int FindSongByBanner()
{
    const char* const* banner = (const char* const*)fo2::kMusicBanner;
    if (banner && banner[1] && *banner[1] && banner[2] && *banner[2])
    {
        const char* artist = banner[1];
        const char* song = banner[2];
        return FindSong(artist ? artist : "", song);
    }
    return -1;
}

// ---- the playing song ----------------------------------------------------------------------------

void OnPlay(int playlist, int lenBytes, const void* data)
{
    (void)lenBytes;
    g_playing = false;
    g_bannerPending = false;
    g_path.clear();
    g_startPos = 0.0;
    g_position = 0.0;
    g_song = -1;
    g_racing = playlist == 2 || playlist == 3;
    if (!g_songsRead || g_songs.empty()) ReadSongs();

    std::string artist, title;
    if (GetSongFromPlaylist(playlist, data, artist, title))
    {
        int song = FindSong(artist, title);
        if (song >= 0)
        {
            g_songAt = NowSeconds();
            g_song = song;
            g_playing = true;
            g_position = 0.0;
            Log("lyrics: \"%s - %s\" (%d lines)%s", g_songs[song].artist.c_str(), g_songs[song].title.c_str(),
                (int)g_songs[song].lines.size(), g_racing ? "" : " - the menus' playlist");
            return;
        }
        else
        {
            Log("lyrics: song \"%s - %s\" not found in lyrics\\", artist.c_str(), title.c_str());
        }
    }
    g_bannerPending = !g_songs.empty();
    g_pendingData = data;
    g_pendingPlaylist = playlist;
}

void __stdcall SongStart(int playlist, int lenBytes, const void* data)
{
    OnPlay(playlist, lenBytes, data);
}

void __stdcall SongStarted()
{
    g_position = 0.0;
    g_startPos = 0.0;
    if (!g_playing && g_bannerPending)
    {
        int song = FindSongByBanner();
        if (song >= 0)
        {
            g_bannerPending = false;
            g_songAt = NowSeconds();
            g_song = song;
            g_playing = true;
            g_position = 0.0;
            Log("lyrics: \"%s - %s\" (%d lines) [from banner]%s", g_songs[song].artist.c_str(), g_songs[song].title.c_str(),
                (int)g_songs[song].lines.size(), g_racing ? "" : " - the menus' playlist");
        }
    }
}

// Before the game's own call: its registers and its stack as they are. The data pointer is the one pushed
// argument ([esp+4] before pushad); the playlist id in eax and the data length in ecx.
static __declspec(naked) void MusicPlayHook()
{
    __asm {
        pushad
        push dword ptr [esp + 32 + 4]  ; data pointer
        push ecx                       ; file length in bytes
        push eax                       ; playlist's id
        call SongStart                 ; __stdcall: cleans 12 bytes
        popad
        push dword ptr [esp + 4]       ; re-push data pointer for oMusicPlay (cdecl)
        call dword ptr [oMusicPlay]
        add esp, 4
        pushad
        call SongStarted
        popad
        ret
    }
}

}  // namespace

bool Install()
{
    if (!g_cfg.vrEnabled) return true;
    bool ok = true;
    for (uintptr_t site : fo2::kMusicPlayCall)
        ok &= PatchCall(site, fo2::kMusicPlay, (const void*)&MusicPlayHook);
    if (!ok)
    {
        Log("lyrics: the game's music play calls differ - no lyrics");
        return false;
    }
    Log("lyrics: the game's music play calls hooked (lyrics from scripts\\lyrics\\*.lrc, [Lyrics] Lyrics=%d)",
        g_cfg.lyricsEnabled ? 1 : 0);
    return true;
}

void Toggle()
{
    g_cfg.lyricsEnabled = !g_cfg.lyricsEnabled;
    SaveIniValue("Lyrics", "Lyrics", g_cfg.lyricsEnabled ? "1" : "0");
    Log("Lyrics key: Lyrics=%d (%s)", g_cfg.lyricsEnabled ? 1 : 0, g_cfg.lyricsEnabled ? "on" : "off");
}

// The song's clock: the playing states advance it (see game.h), the others hold it.
void Tick(double dt)
{
    if (g_bannerPending)
    {
        int song = FindSongByBanner();
        if (song >= 0)
        {
            g_bannerPending = false;
            g_songAt = NowSeconds();
            g_song = song;
            g_playing = true;
            g_position = 0.0;
            Log("lyrics: \"%s - %s\" (%d lines) [from banner]%s", g_songs[song].artist.c_str(), g_songs[song].title.c_str(),
                (int)g_songs[song].lines.size(), g_racing ? "" : " - the menus' playlist");
        }
    }
    if (!g_playing) return;
    int state = *(const int*)fo2::kMusicState;
    if (state >= 1 && state <= 4) g_position += dt;
    else if (state == 0) g_playing = false;
    if (g_position < 0) g_position = 0;
}

const Show& CurrentShow()
{
    static Show none;
    if (!g_playing || g_song < 0 || g_song >= (int)g_songs.size()) return none;
    int state = *(const int*)fo2::kMusicState;
    if (state == 0) return none;
    const std::vector<Line>& lines = g_songs[g_song].lines;
    if (lines.empty()) return none;
    double at = g_position;
    if (at > lines.back().until + kFade) return none;

    // The line being sung: the last one at or before the song's position.
    int i = (int)(std::upper_bound(lines.begin(), lines.end(), at, [](double t, const Line& l) { return t < l.at; }) - lines.begin()) - 1;
    static Show show;
    show.count = 0;
    int max = g_cfg.lyricsLines;
    if (max <= 0) max = 1;
    if (max > 4) max = 4;

    if (i < 0)
    {
        // Ahead of the first line (intro): show line 0 if within 5 seconds.
        if (lines[0].at - at <= 5.0) show.lines[show.count++] = lines[0];
        return show;
    }

    // If the last sung line has finished its until time (+ fade):
    if (at > lines[i].until + kFade)
    {
        // Instrumental break: show upcoming line if within 5 seconds, else hide.
        if (i + 1 < (int)lines.size() && lines[i + 1].at - at <= 5.0)
        {
            show.lines[show.count++] = lines[i + 1];
            return show;
        }
        return none;
    }

    // Active line: include upcoming line if available and max >= 2
    int last = (max >= 2 && i + 1 < (int)lines.size() && lines[i + 1].at - at <= 8.0) ? i + 1 : i;
    int first = std::max(0, last - (max - 1));
    for (int k = first; k <= last && show.count < max; k++)
        show.lines[show.count++] = lines[k];
    return show;
}

// ---- the font -------------------------------------------------------------------------------------

namespace {

// One texture with every letter the .lrc files use, drawn with GDI (its glyphs have proper kerning for
// the letters' widths; ClearType's sub-pixel edges are kept out - they need the screen's layout).
constexpr int kFontPx = 48;  // the letters' height in the atlas, wide enough to be scaled up
constexpr int kAtlas = 1024;
struct Glyph
{
    RECT uv;       // in the atlas' pixels
    float advance; // the letter's width
};
struct Font
{
    IDirect3DTexture9* tex = nullptr;
    Glyph glyph[65536] = {};  // by wchar_t (the atlas holds what the .lrc files use)
    int lineHeight = 0;
} g_font;

// The letters' place in the atlas: filled in the order the songs' lines name them.
bool BuildFont(IDirect3DDevice9* dev)
{
    if (g_font.tex) return true;
    // What the .lrc files use: everything their lines hold.
    bool use[65536] = {};
    bool any = false;
    for (const Song& s : g_songs)
        for (const Line& l : s.lines)
        {
            for (wchar_t c : l.plain) use[c] = true, any = true;
            for (int i = 0; i < l.wordCount; i++)
                for (wchar_t c : l.words[i].text) use[c] = true, any = true;
        }
    if (!any) return false;
    use[L' '] = true;  // the space between the words, always
    HDC dc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(dc);
    HFONT font = CreateFontW(-kFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    if (!font)
    {
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(mem, &tm);
    int cellH = std::max((int)tm.tmHeight, kFontPx);
    g_font.lineHeight = cellH;

    // The atlas: a 32-bit texture of the letters' coverage (white, their alpha the glyph's shape).
    D3DLOCKED_RECT lock = {};
    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = dev->CreateTexture(kAtlas, kAtlas, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    if (FAILED(hr))
        hr = dev->CreateTexture(kAtlas, kAtlas, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
    if (FAILED(hr) || FAILED(tex->LockRect(0, &lock, nullptr, 0)))
    {
        Log("lyrics: BuildFont failed to create/lock texture (hr 0x%08X)", (unsigned int)hr);
        if (tex) tex->Release();
        SelectObject(mem, old);
        DeleteObject(font);
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
        return false;
    }
    DWORD* px = (DWORD*)lock.pBits;
    for (int i = 0; i < kAtlas * (int)lock.Pitch / 4; i++) px[i] = 0;
    // Reserve a 4x4 solid white block at top-left (0,0)-(3,3) for drawing solid colored quads (backing plates, etc.)
    int pitchDwords = (int)lock.Pitch / 4;
    for (int py = 0; py < 4; py++)
        for (int px_idx = 0; px_idx < 4; px_idx++)
            px[py * pitchDwords + px_idx] = 0xFFFFFFFF;
    int x = 6, y = 1, row = 0;
    for (int c = 1; c < 65536; c++)
    {
        if (!use[c]) continue;
        ABC abc = {};
        wchar_t ch = (wchar_t)c;
        if (!GetCharABCWidthsW(mem, ch, ch, &abc)) continue;
        int width = abc.abcA + (int)abc.abcB + abc.abcC;
        if (width <= 0) width = abc.abcB > 0 ? (int)abc.abcB : kFontPx / 4;
        g_font.glyph[c].advance = (float)width;

        GLYPHMETRICS gm = {};
        static MAT2 unit = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
        DWORD need = GetGlyphOutlineW(mem, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &unit);
        if (need == GDI_ERROR || need == 0) continue;
        std::vector<BYTE> bits(need);
        if (GetGlyphOutlineW(mem, ch, GGO_GRAY8_BITMAP, &gm, need, bits.data(), &unit) == GDI_ERROR) continue;

        int cellW = std::max(width, std::max(0, (int)gm.gmptGlyphOrigin.x) + (int)gm.gmBlackBoxX);
        if (x + cellW + 1 > kAtlas)
        {
            x = 1;
            y += row + 1;
            row = 0;
        }
        if (y + cellH + 1 > kAtlas) break;

        // GGO_GRAY8_BITMAP rows are DWORD-aligned, with grey values 0..64.
        int gw = std::min((int)gm.gmBlackBoxX, kAtlas), gh = std::min((int)gm.gmBlackBoxY, kAtlas);
        int stride = ((int)gm.gmBlackBoxX + 3) & ~3;
        int gox = x + std::max(0, (int)gm.gmptGlyphOrigin.x);
        int goy = y + tm.tmAscent - gm.gmptGlyphOrigin.y;
        for (int r = 0; r < gh; r++)
            for (int col = 0; col < gw; col++)
            {
                int px2 = gox + col, py2 = goy + r;
                if (px2 < x || px2 >= x + cellW || px2 >= kAtlas || py2 < y || py2 >= y + cellH || py2 >= kAtlas) continue;
                DWORD a = r * stride + col < (int)bits.size() ? bits[r * stride + col] : 0;
                a = std::min(a, 64ul) * 255 / 64;
                px[py2 * lock.Pitch / 4 + px2] = 0x00FFFFFF | (a << 24);
            }
        g_font.glyph[c].uv = { x, y, x + cellW, y + cellH };
        x += cellW + 2;
        row = std::max(row, cellH);
    }
    tex->UnlockRect(0);
    SelectObject(mem, old);
    DeleteObject(font);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    g_font.tex = tex;
    Log("lyrics: font built (%d px tall)", cellH);
    return true;
}

float TextWidth(const std::wstring& s)
{
    float w = 0;
    for (wchar_t c : s) w += g_font.glyph[c].advance;
    return w;
}

// A letter's place in the atlas (its top left), as the texture's 0..1 coordinates.
void GlyphUv(wchar_t c, float* u0, float* v0, float* u1, float* v1)
{
    const RECT& r = g_font.glyph[c].uv;
    *u0 = (float)r.left / kAtlas;
    *v0 = (float)r.top / kAtlas;
    *u1 = (float)r.right / kAtlas;
    *v1 = (float)r.bottom / kAtlas;
}

bool InRace()
{
    BYTE* gf = *(BYTE**)fo2::kGameFlow;
    return gf && *(const int*)(gf + fo2::kGameState) == 2;
}

}  // namespace

IDirect3DTexture9* FontTexture(IDirect3DDevice9* dev)
{
    if ((!g_songsRead || g_songs.empty()) && g_song >= 0) ReadSongs();
    if (g_song < 0) return nullptr;
    return BuildFont(dev) ? g_font.tex : nullptr;
}

void ReleaseFont()
{
    if (g_font.tex) g_font.tex->Release();
    g_font = Font();
}

void Draw(IDirect3DDevice9* dev, float x, float y, float w, float h,
          float tanLeft, float tanRight, float tanDown, float tanUp,
          const void* cam)
{
    (void)x; (void)y;
    const Show& show = CurrentShow();
    if (!show.count || !dev) return;
    IDirect3DTexture9* tex = g_font.tex;
    if (!tex && !(tex = FontTexture(dev))) return;

    struct V { float x, y, z, rhw; DWORD colour; float u, v; };
    std::vector<V> verts;
    double now = g_position;

    // Convert config 0x00BBGGRR to D3DCOLOR 0x00RRGGBB.
    auto toRgb = [](uint32_t bgr) -> DWORD {
        return ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
    };
    DWORD dim = toRgb(g_cfg.lyricsColour);
    DWORD bright = toRgb(g_cfg.lyricsSungColour);

    bool drewOnBumper = false;
    if (InRace() && cam)
    {
        BYTE* player = *(BYTE**)fo2::kLocalPlayer;
        BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
        if (car)
        {
            const float* carM = (const float*)(car + fo2::kCarMatrix);
            const float* e = (const float*)((const BYTE*)cam + fo2::kCamMatrix);
            float camNear = *(const float*)((const BYTE*)cam + fo2::kCamNear);

            const float* lo = (const float*)(car + fo2::kCarCabinMin);
            const float* hi = (const float*)(car + fo2::kCarCabinMax);
            const float* com = (const float*)(car + fo2::kCarCentreOfMass);

            // Compute rear bumper position in car's local coordinates.
            float rearZ = -2.25f;
            float bumperY = -0.15f;
            if (lo && hi && com)
            {
                // Check if bottom collision box at car + 0x178 is present and valid
                const float* bLo = (const float*)(car + 0x178);
                if (bLo && bLo[2] < lo[2] - 0.3f && bLo[2] > -5.0f && bLo[2] < 0.0f)
                {
                    rearZ = bLo[2] - com[2];
                    float bottomY = bLo[1] - com[1];
                    float cabinBottomY = lo[1] - com[1];
                    bumperY = bottomY + 0.40f * (cabinBottomY - bottomY);
                }
                else if (lo[2] < 0.0f && lo[2] > -5.0f)
                {
                    rearZ = (lo[2] - com[2]) - 1.10f;
                    float cabinBottomY = lo[1] - com[1];
                    bumperY = cabinBottomY - 0.25f;
                }
            }
            // Vertical fine-tuning via g_cfg.lyricsY (default 0.60; larger lyricsY moves lower):
            bumperY += (0.60f - g_cfg.lyricsY) * 0.8f;
            // Float 3 cm off the bumper surface along car's backward vector:
            rearZ -= 0.03f;

            // Bumper center in world coordinates:
            // carM[0..2] is Right, carM[4..6] is Up, carM[8..10] is Forward, carM[12..14] is Position
            float bumperCenter[3];
            for (int i = 0; i < 3; i++)
                bumperCenter[i] = carM[12 + i] + bumperY * carM[4 + i] + rearZ * carM[8 + i];

            // Vector from bumper center to eye
            float toEye[3] = { e[12] - bumperCenter[0], e[13] - bumperCenter[1], e[14] - bumperCenter[2] };
            float eyeDistSq = toEye[0] * toEye[0] + toEye[1] * toEye[1] + toEye[2] * toEye[2];
            float eyeDist = sqrtf(eyeDistSq);

            // Bumper outward normal is -Forward (-carM[8..10]).
            // Camera faces bumper if vector to eye has positive dot product with -carMForward.
            float dotNormal = -(toEye[0] * carM[8] + toEye[1] * carM[9] + toEye[2] * carM[10]);
            float facing = eyeDist > 0.1f ? dotNormal / eyeDist : 0.0f;

            if (facing > 0.05f && eyeDist > 0.3f && eyeDist < 25.0f)
            {
                // Chase camera is behind the car looking at the rear bumper!
                auto toWorld = [&](float lx, float ly, float lz, float* out) {
                    for (int i = 0; i < 3; i++)
                        out[i] = carM[12 + i] + lx * carM[i] + ly * carM[4 + i] + lz * carM[8 + i];
                };

                auto project3D = [&](const float* p, float u, float v, DWORD col, V& out) -> bool {
                    float d[3] = { p[0] - e[12], p[1] - e[13], p[2] - e[14] };
                    float cx = d[0] * e[0] + d[1] * e[1] + d[2] * e[2];
                    float cy = d[0] * e[4] + d[1] * e[5] + d[2] * e[6];
                    float cz = d[0] * e[8] + d[1] * e[9] + d[2] * e[10];
                    if (cz < camNear * 1.01f) return false;
                    out.x = (cx / cz - tanLeft) / (tanRight - tanLeft) * w - 0.5f;
                    out.y = (tanUp - cy / cz) / (tanUp - tanDown) * h - 0.5f;
                    out.z = 0.0f;
                    out.rhw = 1.0f / cz;
                    out.colour = col;
                    out.u = u;
                    out.v = v;
                    return true;
                };

                auto emitQuad3D = [&](float x0, float y0, float x1, float y1, float lz,
                                      float u0, float v0, float u1, float v1, DWORD col) {
                    float pBL[3], pBR[3], pTL[3], pTR[3];
                    toWorld(x0, y0, lz, pBL);
                    toWorld(x1, y0, lz, pBR);
                    toWorld(x0, y1, lz, pTL);
                    toWorld(x1, y1, lz, pTR);
                    V vBL, vBR, vTL, vTR;
                    if (project3D(pBL, u0, v1, col, vBL) &&
                        project3D(pBR, u1, v1, col, vBR) &&
                        project3D(pTL, u0, v0, col, vTL) &&
                        project3D(pTR, u1, v0, col, vTR))
                    {
                        verts.push_back(vTL);
                        verts.push_back(vTR);
                        verts.push_back(vBL);
                        verts.push_back(vTR);
                        verts.push_back(vBR);
                        verts.push_back(vBL);
                    }
                };

                float lineH3D = 0.075f * g_cfg.lyricsScale;
                float cellH = g_font.lineHeight > 0 ? (float)g_font.lineHeight : (float)kFontPx;
                float scale3D = lineH3D / cellH;

                float lineWidths[6] = {};
                float lineAlphas[6] = {};
                float maxLineW = 0.0f;
                float maxAlpha = 0.0f;

                for (int i = 0; i < show.count; i++)
                {
                    const Line& line = show.lines[i];
                    float alpha = 1.0f;
                    if (now < line.at)
                    {
                        double appearAt = i > 0 ? show.lines[i - 1].at : line.at - 4.0;
                        if (now < appearAt) alpha = 0.0f;
                        else if (now - appearAt < kFade) alpha = (float)((now - appearAt) / kFade);
                    }
                    else if (now - line.at < kFade && i == show.count - 1)
                    {
                        alpha = (float)((now - line.at) / kFade);
                    }
                    if (now > line.until)
                    {
                        double past = now - line.until;
                        alpha = past >= kFade ? 0.0f : std::min(alpha, (float)(1.0 - past / kFade));
                    }
                    lineAlphas[i] = alpha;
                    if (alpha > maxAlpha) maxAlpha = alpha;
                    if (alpha > 0.0f)
                    {
                        lineWidths[i] = TextWidth(line.plain) * scale3D;
                        if (lineWidths[i] > maxLineW) maxLineW = lineWidths[i];
                    }
                }

                // If line is wider than typical rear bumper area (1.35m), shrink scale to fit neatly:
                float maxAllowedW = 1.35f;
                if (maxLineW > maxAllowedW && maxLineW > 0.0f)
                {
                    float shrink = maxAllowedW / maxLineW;
                    scale3D *= shrink;
                    lineH3D *= shrink;
                    for (int i = 0; i < show.count; i++)
                        lineWidths[i] *= shrink;
                    maxLineW = maxAllowedW;
                }

                if (maxAlpha > 0.0f)
                {
                    // Backing plate and border:
                    float plateW = std::clamp(maxLineW + 0.12f, 0.40f, 1.45f);
                    float plateH = show.count * lineH3D + 0.035f;
                    float px0 = -0.5f * plateW;
                    float px1 = 0.5f * plateW;
                    float py0 = bumperY - 0.5f * plateH;
                    float py1 = bumperY + 0.5f * plateH;
                    float pz = rearZ;
                    float textZ = rearZ - 0.005f;

                    float uw = 1.5f / (float)kAtlas, vw = 1.5f / (float)kAtlas;
                    DWORD plateA = (DWORD)std::clamp(lroundf(maxAlpha * 220.0f), 0l, 255l) << 24;
                    DWORD plateCol = plateA | 0x00101418;
                    DWORD borderCol = plateA | 0x00040608;

                    // Draw outer border rim (0.008m larger) then translucent dark plate
                    emitQuad3D(px0 - 0.008f, py0 - 0.008f, px1 + 0.008f, py1 + 0.008f, pz + 0.001f,
                               uw, vw, uw, vw, borderCol);
                    emitQuad3D(px0, py0, px1, py1, pz, uw, vw, uw, vw, plateCol);

                    auto emitChar3D = [&](wchar_t ch, float cx, float y0, DWORD c) {
                        const Glyph& g = g_font.glyph[ch];
                        float gw = (float)(g.uv.right - g.uv.left) * scale3D;
                        if (gw > 0)
                        {
                            float u0, v0, u1, v1;
                            GlyphUv(ch, &u0, &v0, &u1, &v1);
                            emitQuad3D(cx, y0, cx + gw, y0 + lineH3D, textZ, u0, v0, u1, v1, c);
                        }
                    };

                    float totalTextH = show.count * lineH3D;
                    for (int i = 0; i < show.count; i++)
                    {
                        float alpha = lineAlphas[i];
                        if (alpha <= 0.0f) continue;
                        const Line& line = show.lines[i];
                        float lineY0 = (bumperY + 0.5f * totalTextH) - (i + 1) * lineH3D;
                        float startX = -0.5f * lineWidths[i];
                        DWORD aBits = (DWORD)std::clamp(lroundf(alpha * 255.0f), 0l, 255l) << 24;

                        // Outline shadow in 3D:
                        float o = 1.5f * scale3D;
                        DWORD shadowColor = 0xFF000000 | (aBits & 0xFF000000);
                        const float offsets[4][2] = { { -o, 0 }, { o, 0 }, { 0, -o }, { 0, o } };
                        for (const auto& off : offsets)
                        {
                            float scx = startX + off[0];
                            float sy0 = lineY0 + off[1];
                            if (line.wordCount > 0)
                            {
                                for (int wIdx = 0; wIdx < line.wordCount; wIdx++)
                                    for (wchar_t ch : line.words[wIdx].text)
                                    {
                                        emitChar3D(ch, scx, sy0, shadowColor);
                                        scx += g_font.glyph[ch].advance * scale3D;
                                    }
                            }
                            else
                            {
                                for (wchar_t ch : line.plain)
                                {
                                    emitChar3D(ch, scx, sy0, shadowColor);
                                    scx += g_font.glyph[ch].advance * scale3D;
                                }
                            }
                        }

                        // Colored text in 3D:
                        float scx = startX;
                        if (line.wordCount > 0)
                        {
                            for (int wIdx = 0; wIdx < line.wordCount; wIdx++)
                            {
                                const Word& word = line.words[wIdx];
                                DWORD c = (now >= word.at ? bright : dim) | aBits;
                                for (wchar_t ch : word.text)
                                {
                                    emitChar3D(ch, scx, lineY0, c);
                                    scx += g_font.glyph[ch].advance * scale3D;
                                }
                            }
                        }
                        else
                        {
                            DWORD c = (now >= line.at ? bright : dim) | aBits;
                            for (wchar_t ch : line.plain)
                            {
                                emitChar3D(ch, scx, lineY0, c);
                                scx += g_font.glyph[ch].advance * scale3D;
                            }
                        }
                    }

                    if (!verts.empty())
                        drewOnBumper = true;
                }
            }
        }
    }

    if (!drewOnBumper)
    {
        // 2D screen overlay at bottom of screen
        float lineH = 0.052f * h * g_cfg.lyricsScale;
        float cellH = g_font.lineHeight > 0 ? (float)g_font.lineHeight : (float)kFontPx;
        float scale = lineH / cellH;  // atlas pixels into the eye's
        float yFrac = InRace() ? g_cfg.lyricsY : 0.82f;
        float cy = h * yFrac;
        auto emitChar = [&](wchar_t ch, float cx, float y0, DWORD c) {
            const Glyph& g = g_font.glyph[ch];
            float gw = (float)(g.uv.right - g.uv.left) * scale;
            if (gw > 0)
            {
                float u0, v0, u1, v1;
                GlyphUv(ch, &u0, &v0, &u1, &v1);
                float x0 = cx, x1 = cx + gw, y1 = y0 + lineH;
                verts.push_back({ x0, y0, 0, 1, c, u0, v0 });
                verts.push_back({ x1, y0, 0, 1, c, u1, v0 });
                verts.push_back({ x0, y1, 0, 1, c, u0, v1 });
                verts.push_back({ x1, y0, 0, 1, c, u1, v0 });
                verts.push_back({ x1, y1, 0, 1, c, u1, v1 });
                verts.push_back({ x0, y1, 0, 1, c, u0, v1 });
            }
        };
        for (int i = show.count - 1; i >= 0; i--, cy -= lineH)
        {
            const Line& line = show.lines[i];
            // Fade the line in when it appears, and fade out after its until time when it is the last shown.
            float alpha = 1.0f;
            if (now < line.at)
            {
                // Upcoming preview line: fade in from when the previous line started (or 4s before the first line).
                double appearAt = i > 0 ? show.lines[i - 1].at : line.at - 4.0;
                if (now < appearAt) alpha = 0.0f;
                else if (now - appearAt < kFade) alpha = (float)((now - appearAt) / kFade);
            }
            else if (now - line.at < kFade && i == show.count - 1)
            {
                alpha = (float)((now - line.at) / kFade);
            }
            if (now > line.until)
            {
                double past = now - line.until;
                alpha = past >= kFade ? 0.0f : std::min(alpha, (float)(1.0 - past / kFade));
            }
            if (alpha <= 0) continue;
            float width = TextWidth(line.plain) * scale;
            float cx = 0.5f * (w - width);
            float y0 = cy - 0.5f * lineH;
            DWORD aBits = (DWORD)std::clamp(lroundf(alpha * 255.0f), 0l, 255l) << 24;

            // Draw crisp 4-way black outline around letters for maximum legibility in VR:
            float o = 1.5f * scale;
            DWORD shadowColor = 0xFF000000 | (aBits & 0xFF000000);
            const float offsets[4][2] = { { -o, 0 }, { o, 0 }, { 0, -o }, { 0, o } };
            for (const auto& off : offsets)
            {
                if (line.wordCount > 0)
                {
                    float scx = cx + off[0];
                    float sy0 = y0 + off[1];
                    for (int wIdx = 0; wIdx < line.wordCount; wIdx++)
                    {
                        const Word& word = line.words[wIdx];
                        for (wchar_t ch : word.text)
                        {
                            emitChar(ch, scx, sy0, shadowColor);
                            scx += g_font.glyph[ch].advance * scale;
                        }
                    }
                }
                else
                {
                    float scx = cx + off[0];
                    float sy0 = y0 + off[1];
                    for (wchar_t ch : line.plain)
                    {
                        emitChar(ch, scx, sy0, shadowColor);
                        scx += g_font.glyph[ch].advance * scale;
                    }
                }
            }

            // The words with their own times brighten one by one; else the whole line at its time.
            if (line.wordCount > 0)
            {
                for (int wIdx = 0; wIdx < line.wordCount; wIdx++)
                {
                    const Word& word = line.words[wIdx];
                    DWORD c = (now >= word.at ? bright : dim) | aBits;
                    for (wchar_t ch : word.text)
                    {
                        emitChar(ch, cx, y0, c);
                        cx += g_font.glyph[ch].advance * scale;
                    }
                }
            }
            else
            {
                DWORD c = (now >= line.at ? bright : dim) | aBits;
                for (wchar_t ch : line.plain)
                {
                    emitChar(ch, cx, y0, c);
                    cx += g_font.glyph[ch].advance * scale;
                }
            }
        }
    }
    if (verts.empty()) return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;

    D3DVIEWPORT9 oldVp = {};
    dev->GetViewport(&oldVp);
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

    bool beganScene = SUCCEEDED(dev->BeginScene());
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(verts.size() / 3), verts.data(), sizeof(V));
    if (beganScene) dev->EndScene();

    dev->SetViewport(&oldVp);
    saved->Apply();
    saved->Release();
}

}  // namespace lyrics
