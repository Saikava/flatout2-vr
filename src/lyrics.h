#pragma once
// The songs' lyrics, karaoke style, in the headset (see lyrics.cpp).
#include <cstdint>
#include <string>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace lyrics {

// Hooks the game's music play (its playlist advance). False when the game's code differs - no lyrics then.
bool Install();

// Advances the song's clock and picks the lines to show; called once per frame with the seconds since
// the last call.
void Tick(double dt);

// Toggles lyrics display on/off and writes the setting to FO2VR.ini.
void Toggle();

// The lines of the playing song, in order, with the words not yet sung and those already sung (when the
// .lrc file tells each word's time): up to `linesShown` lines around the song's position, each faded in
// and out over its first and last kFade seconds. An empty out when nothing is to show.
struct Word
{
    std::wstring text;     // with the trailing space when the line keeps it
    double at = 0;         // when it is sung, seconds into the song (when the file tells it)
};
struct Line
{
    std::wstring plain;    // the line's text, measured and drawn
    double at = 0;         // when it is sung, seconds into the song
    double until = 0;       // the next line's time (its own fade starts there)
    Word words[64];        // the words with their own times (empty text: the line has none)
    int wordCount = 0;
};
struct Show
{
    Line lines[6];
    int count = 0;
};
const Show& CurrentShow();

// A bitmap font (one texture, an atlas of the letters the .lrc files use), drawn with the eyes' own
// draw calls. Built lazily on the first use, released with the VR targets on a device reset.
IDirect3DTexture9* FontTexture(IDirect3DDevice9* dev);
void ReleaseFont();

// Draws the lyrics into the bound render target: x, y, w, h in pixels.
// In races with HUD active, drawn as a static part of the UI in the lower center between
// the character interface on the left and the speedometer on the right.
void Draw(IDirect3DDevice9* dev, float x, float y, float w, float h,
          float tanLeft = -1.0f, float tanRight = 1.0f, float tanDown = -1.0f, float tanUp = 1.0f);

}  // namespace lyrics
