#ifndef GUARD_FIRST_PERSON_H
#define GUARD_FIRST_PERSON_H

#include "config/overworld.h"

#if OW_FIRST_PERSON
bool32 FirstPerson_Update(u16 *newKeys, u16 *heldKeys); // once per frame from the overworld CB1 (may rewrite the keys); TRUE = skip the rest of this CB1 frame
void FirstPerson_VBlank(void);          // from the field VBlank callback
void FirstPerson_HeapReset(void);       // from InitHeap: forget the (now invalid) heap block
bool8 FirstPerson_IsActive(void);
bool8 FirstPerson_VramLocked(void);
void FirstPerson_EndOfInput(void);      // after the field input ran: drop the emulated "finished a step" avatar state
u8 FirstPerson_LedgeDir(void);   // free movement pushed into a ledge: direction for PlayerStep (real ledge jump), else DIR_NONE
bool8 FirstPerson_FreeMovement(void);   // free movement is driving the player: the normal PlayerStep must not move the avatar     // TRUE while the bitmap pages own VRAM (skip tileset animation uploads)
#else
static inline bool32 FirstPerson_Update(u16 *newKeys, u16 *heldKeys) { return FALSE; }
static inline void FirstPerson_VBlank(void) {}
static inline void FirstPerson_HeapReset(void) {}
static inline bool8 FirstPerson_IsActive(void) { return FALSE; }
static inline bool8 FirstPerson_VramLocked(void) { return FALSE; }
static inline void FirstPerson_EndOfInput(void) {}
static inline u8 FirstPerson_LedgeDir(void) { return 0; }
static inline bool8 FirstPerson_FreeMovement(void) { return FALSE; }
#endif

#endif // GUARD_FIRST_PERSON_H
