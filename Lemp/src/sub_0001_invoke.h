#ifndef __SUB_0001_BOOK_FACE_V5_INVOKE_H__
#define __SUB_0001_BOOK_FACE_V5_INVOKE_H__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Declare arenas
extern uint8_t sub_0001_book_face_v5_arena[786432];

// Fast scratch arena not used for Ethos-U55
// We will not create it for now and reuse the address of the other arena
extern uint8_t* sub_0001_book_face_v5_fast_scratch; // size: 786432

int sub_0001_book_face_v5_invoke(bool clean_outputs);


#endif // __SUB_0001_BOOK_FACE_V5_INVOKE_H__
