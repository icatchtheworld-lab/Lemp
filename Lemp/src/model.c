#include "model.h"

#include <stddef.h>

#include "sub_0001_invoke.h"

int8_t * GetModelInputPtr_book_face_v5_images(void)
{
    return (int8_t *) (sub_0001_book_face_v5_arena + sub_0001_book_face_v5_address_images_70592_11134);
}

int8_t const * GetModelOutputPtr_book_face_v5_output0_70440_70594(void)
{
    return (int8_t const *)
        (sub_0001_book_face_v5_arena + sub_0001_book_face_v5_address_output0_70440_70594_11138);
}

int8_t const * GetModelOutputPtr_book_face_v5__837_70448_70593(void)
{
    return (int8_t const *)
        (sub_0001_book_face_v5_arena + sub_0001_book_face_v5_address__837_70448_70593_11130);
}

int RunModel_book_face_v5(bool clean_outputs)
{
    return sub_0001_book_face_v5_invoke(clean_outputs);
}
