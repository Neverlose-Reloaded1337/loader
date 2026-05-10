#pragma once

#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#pragma pack(push, 1)

struct struct_C8 {
    uint32_t field_0;
    void* field_4;
    uint32_t field_8;
    uint32_t field_C;
};

struct UI_Element_Base {
    uint32_t field_0;
    void* field_4;
    __m128i field_8;

    // 🔥 IMPORTANT: make this const-safe
    char* str_ptr;
    uint32_t str_len;
    uint32_t field_3C;
};

struct struct_1B8 {
    uint32_t field_0;
    void* field_4;
    uint32_t field_8;
    uint32_t field_C;
};

struct struct_5F0 {
    uint32_t field_0;
    void* field_4;
    uint32_t field_8;
    uint32_t field_C;
    uint32_t field_10;
};

class Menu {
public:
    uint32_t vtable;

    uint8_t pad_0[0xC8 - 4];
    struct_C8 m_c8;

    uint8_t pad_1[0x1B8 - (0xC8 + sizeof(struct_C8))];
    struct_1B8 m_1b8;

    uint8_t pad_2[0x2A8 - (0x1B8 + sizeof(struct_1B8))];
    UI_Element_Base m_2a8;

    uint8_t pad_3[0x3C0 - (0x2A8 + sizeof(UI_Element_Base))];
    UI_Element_Base m_3c0;

    uint8_t pad_4[0x4D8 - (0x3C0 + sizeof(UI_Element_Base))];
    UI_Element_Base m_4d8;

    uint8_t pad_5[0x5F0 - (0x4D8 + sizeof(UI_Element_Base))];
    struct_5F0 m_5f0;

    uint8_t padding[64];

    // 🔥 IMPORTANT: declare correctly
    Menu* sub_412CE650();

    void sub_412EFBE0();
    void sub_412CF8D0(struct_C8* p);
    void sub_412E6310(struct_1B8* p);
    void sub_41317660(UI_Element_Base* p);
    void sub_4130C7C0(struct struct_5F0* p);
};

#pragma pack(pop)