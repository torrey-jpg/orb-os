#pragma once
#include <stdint.h>

// Kevin's Orb — the pre-rendered 3D teeth on the SD card, shared by the Tooth Explorer and
// the hologram screensaver.
//
// /teeth/tNN.bin (tools/teeth/render_teeth.py), one file per tooth type:
//   "TTH2" u16 yaw_count u16 pitch_count u16 w u16 h,
//   then yaw_count*pitch_count x (u32 offset, u32 length), frame = pitch*yaw_count + yaw,
//   then the PNGs.
// About 5-7 MB per tooth: too big to hold, so open() reads the table and decode() reads one
// picture at a time from the card.
//
// The pictures are the patient's right-side teeth. A left-side tooth is the same picture
// flipped left-to-right; decode() does the flip, and frameFor() turns yaw the other way for
// a flipped tooth so it still spins the way a finger drags it.
class ToothFrames {
public:
    enum Tint { NATURAL, HOLOGRAM };

    ~ToothFrames() { close(); }

    bool open(int type);              // read /teeth/tNN.bin's header and table
    void close();                     // close the file, free the table and the decoder
    bool isOpen() const { return m_open; }
    int  type()   const { return m_type; }
    int  width()  const { return m_w; }
    int  height() const { return m_h; }
    int  yawCount()   const { return m_nyaw; }
    int  pitchCount() const { return m_npitch; }

    // Frame index for an angle (any integers; both wrap), flipped for a left-side tooth.
    int  frameFor(int yaw, int pitch, bool flip) const;

    // Decode one frame into `dst` (width*height RGB565, caller-owned). HOLOGRAM recolours it
    // by brightness into the screensaver's mint glow.
    bool decode(int frame, bool flip, uint16_t *dst, Tint tint = NATURAL);

private:
    bool readAt(uint32_t off, uint8_t *dst, uint32_t len);

    bool      m_open = false;
    int       m_type = -1;
    int       m_nyaw = 0, m_npitch = 0, m_w = 0, m_h = 0;
    uint32_t *m_table = nullptr;     // (offset, length) per frame
    uint8_t  *m_comp = nullptr;      // one compressed frame
    uint32_t  m_compCap = 0;
    void     *m_file = nullptr;      // File* on the device, FILE* in the simulator
    void     *m_png = nullptr;       // PNGdec decoder, made on first decode
};
