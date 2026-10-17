// Frames from the ZiiUScreen virtual monitor (driver/ziiu_display, an IddCx driver
// running in WUDFHost) to the GamePad streamer (tools/ziiu-drc).
//
// A named section holds a header and ZIIU_FRAME_SLOTS frame slots (BGRA, top-down).
// The driver writes the frame into slot (seq % ZIIU_FRAME_SLOTS), stores seq in
// that slot's SlotSeq, then publishes seq in Latest and signals the event. A reader
// copies the slot named by Latest and accepts the copy only if SlotSeq still holds
// the same seq afterwards (the writer is at most one slot ahead, never on it).
#pragma once
#include <stdint.h>

#define ZIIU_FRAME_SECTION     L"Global\\ZiiUScreenFrames"
#define ZIIU_FRAME_EVENT       L"Global\\ZiiUScreenFrameReady"
#define ZIIU_FRAME_MAGIC       0x4655495au   // "ZIUF"
#define ZIIU_FRAME_VERSION     1u
#define ZIIU_FRAME_SLOTS       3u
#define ZIIU_FRAME_MAX_WIDTH   1920u        // section sized for this; modes are smaller
#define ZIIU_FRAME_MAX_HEIGHT  1080u
#define ZIIU_FRAME_SLOT_BYTES  (ZIIU_FRAME_MAX_WIDTH * ZIIU_FRAME_MAX_HEIGHT * 4u)

typedef struct _ZIIU_FRAME_SLOT_INFO {
    volatile int64_t Seq;         // frame number written to this slot (0 = never)
    uint32_t Width, Height;       // pixels
    uint32_t Stride;              // bytes per row in the slot (Width * 4)
    uint32_t DirtyRects;          // as reported by IddCx for this frame (0 = full)
    int64_t  QpcAcquired;         // QueryPerformanceCounter when the driver got the frame
    uint64_t PresentFrame;        // IddCx PresentationFrameNumber
} ZIIU_FRAME_SLOT_INFO;

typedef struct _ZIIU_FRAME_HEADER {
    uint32_t Magic;               // ZIIU_FRAME_MAGIC
    uint32_t Version;             // ZIIU_FRAME_VERSION
    uint32_t Slots;               // ZIIU_FRAME_SLOTS
    uint32_t SlotBytes;           // ZIIU_FRAME_SLOT_BYTES
    uint32_t DataOffset;          // offset of slot 0 from the start of the section
    uint32_t Reserved;
    volatile int64_t Latest;      // last complete frame (0 = none yet)
    volatile int64_t Dropped;     // frames the driver could not copy (map failures)
    ZIIU_FRAME_SLOT_INFO Slot[ZIIU_FRAME_SLOTS];
} ZIIU_FRAME_HEADER;

#define ZIIU_FRAME_DATA_OFFSET 4096u
#define ZIIU_FRAME_SECTION_BYTES (ZIIU_FRAME_DATA_OFFSET + ZIIU_FRAME_SLOTS * ZIIU_FRAME_SLOT_BYTES)
