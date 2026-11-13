/*
    bi_text.c
    AerLang's built-in functions for text and numbers: print, write and input, strings (len,
    sub, find, upper, split, join...), turning one kind of value into another (str, int, float,
    chr, ord) and maths (sqrt, sin, cos, atan2, floor, round, abs).

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_ctype.h"
#include "../lib/aer_math.h"
#include "../lib/crypto.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"

// printing and input

// show every value the way print() does, with one space between each (shared by print and write)
static void print_values (int argc, value_t* argv) {
    for (int i = 0; i < argc; i++) {
        // a space before every value but the first, so there is none at the end of the line
        if (i > 0) {
            console_putchar_raw(' ');
        }
        aer_print_value(argv[i]);
    }
}

// print(a, b, ...): show the values with a space between each, then start a new line
value_t bi_print (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    console_putchar('\n');
    return aer_nil();
}

// write(a, b, ...): like print(), but stays on the same line
value_t bi_write (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    // the values were written without moving the blinking cursor (print's '\n' does that for
    // it), so move it to the end of the text by hand
    console_update_cursor();
    return aer_nil();
}

// input(prompt): show the prompt (if there is one), wait for a line to be typed, and give it back
value_t bi_input (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) {
        aer_print_value(argv[0]);
    }
    (void)line;
    // getln stops taking characters when the buffer is full, so a line is at most 255 characters
    getln(buffer, sizeof(buffer));
    // aer_string copies the text, so the buffer on the stack can go when we return
    return aer_string(buffer, (uint32_t)slen(buffer));
}

// inpw(prompt): like input(), but shows * for each character (for passwords)
value_t bi_input_secret (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) {
        aer_print_value(argv[0]);
    }
    (void)line;
    GetSecret(buffer, sizeof(buffer));
    value_t s = aer_string(buffer, (uint32_t)slen(buffer));
    // the program now has its own copy, so blank ours: a password must not be left lying in
    // stack memory for whatever runs next to find
    wipe(buffer, sizeof(buffer));
    return s;
}

// strings and conversions

// len(x): how many characters are in a string, items in a list, or bytes in bytes
value_t bi_len (int argc, value_t* argv, int line) {
    (void)argc;
    if (argv[0].type != V_STR && argv[0].type != V_LIST && argv[0].type != V_BYTES) {
        aer_error(line, "len() needs a string, list or bytes, not a %s", aer_type_name(argv[0]));
    }
    // all three kinds of object keep their size in the same length field, so one line does for them all
    return aer_int((int32_t)argv[0].as.obj->length);
}

// str(x): any value as a string, the way print() would show it
value_t bi_str (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    return aer_to_string(argv[0]);
}

// a decimal as a whole number, its decimals cut off (2.7 -> 2, -2.7 -> -2). nil if it is too big
static value_t whole_part (double x) {
    if (!(x > -2147483649.0 && x < 2147483648.0)) {
        return aer_nil(); // (nan fails both tests too)
    }
    return aer_int((int32_t)x);
}

// int(x): x as a whole number. A decimal loses its decimals (2.7 is 2, -2.7 is -2; floor()
// rounds down instead), a string is read as a number ("42" is 42, "0x1F" is 31, "2.5" is 2),
// and true / false are 1 / 0. nil if it isn't a number
value_t bi_int (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    switch (argv[0].type) {
        // a whole number holds no reference, so it can be handed straight back
        case V_INT: return argv[0];
        case V_FLOAT: return whole_part(argv[0].as.f);
        // true and false are stored as 1 and 0 already
        case V_BOOL: return aer_int(argv[0].as.i);
        case V_STR: {
            char* end;
            const char* text = argv[0].as.obj->data.chars;
            // base 0 lets strtol work out the base from the text: "0x1F" is hex, and (beware)
            // a leading 0 means octal, so "010" is 8. end is left on the first character it
            // couldn't use
            long value = strtol(text, &end, 0);
            // stopped at a decimal point or an exponent: the text is really a decimal, "2.5" or "1e3"
            if (*end == '.' || *end == 'e' || *end == 'E') {
                // a decimal: read it as one, then cut it down
                double real;
                const char* real_end;
                if (parse_real(text, &real, &real_end)) {
                    end = (char*)real_end;
                    while (isspace((unsigned char)*end)) {
                        end++;
                    }
                    return *end == '\0' ? whole_part(real) : aer_nil();
                }
            }
            // spaces after the number are fine ("42 " is 42), anything else after it is not
            while (isspace((unsigned char)*end)) {
                end++;
            }
            // end == text means strtol found no digits at all ("" or "abc")
            if (end == text || *end != '\0') {
                return aer_nil(); // not a number
            }
            return aer_int((int32_t)value);
        }
        default: return aer_nil();
    }
}

// float(x): x as a decimal number: 3 is 3.0, "2.5" is 2.5. nil if it isn't a number
value_t bi_float (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    switch (argv[0].type) {
        case V_INT:
        case V_FLOAT: return aer_float(aer_real(argv[0]));
        case V_BOOL: return aer_float(argv[0].as.i);
        case V_STR: {
            // skip spaces in front, so " 2.5" works the same as "2.5"
            const char* text = argv[0].as.obj->data.chars;
            while (isspace((unsigned char)*text)) {
                text++;
            }
            double real;
            const char* end;
            // parse_real reads as much of a number as it can, and says where it stopped
            if (!parse_real(text, &real, &end)) {
                return aer_nil();
            }
            // only spaces may come after the number: "2.5x" isn't a number
            while (isspace((unsigned char)*end)) {
                end++;
            }
            return *end == '\0' ? aer_float(real) : aer_nil();
        }
        default: return aer_nil();
    }
}

// ---- maths -----------------------------------------------------------------------------------------
//
// Angles are in radians: a whole turn is 2 * PI. In a game, the x and y of a step in direction
// `angle` are cos(angle) * speed and sin(angle) * speed

// sqrt(x): the square root of x (nan for a negative x)
value_t bi_sqrt (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_float(sqrt(real_arg(argv, 0, line, "sqrt")));
}

// sin(angle): the sine of an angle in radians
value_t bi_sin (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_float(sin(real_arg(argv, 0, line, "sin")));
}

// cos(angle): the cosine of an angle in radians
value_t bi_cos (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_float(cos(real_arg(argv, 0, line, "cos")));
}

// atan2(y, x): the angle from the origin to the point (x, y), in radians from -PI to PI
value_t bi_atan2 (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_float(atan2(real_arg(argv, 0, line, "atan2"), real_arg(argv, 1, line, "atan2")));
}

// floor(x): the whole number at or below x, as a whole number (2.7 is 2, -2.7 is -3)
value_t bi_floor (int argc, value_t* argv, int line) {
    (void)argc;
    return whole_part(floor(real_arg(argv, 0, line, "floor")));
}

// round(x): the nearest whole number to x, halves going up (2.5 is 3, -2.5 is -2)
value_t bi_round (int argc, value_t* argv, int line) {
    (void)argc;
    // adding a half and rounding down is the same as rounding to the nearest, with halves going up
    return whole_part(floor(real_arg(argv, 0, line, "round") + 0.5));
}

// abs(x): x without its sign, the same kind of number it was (abs(-3) is 3, abs(-2.5) is 2.5)
value_t bi_abs (int argc, value_t* argv, int line) {
    (void)argc;
    // a whole number stays a whole number (beware: -2147483648 has no positive partner in 32 bits)
    if (argv[0].type == V_INT) {
        return aer_int(argv[0].as.i < 0 ? -argv[0].as.i : argv[0].as.i);
    }
    // anything else must be a decimal (real_arg reports a string or a list as a mistake)
    return aer_float(fabs(real_arg(argv, 0, line, "abs")));
}

// chr(n): the character with that code, chr(65) is "A"
value_t bi_chr (int argc, value_t* argv, int line) {
    (void)argc;
    // the cast keeps only the lowest 8 bits, so chr(321) is the same as chr(65)
    char c = (char)int_arg(argv, 0, line, "chr");
    return aer_string(&c, 1);
}

// ord(s): the code of a string's first character, ord("A") is 65
value_t bi_ord (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* s = str_arg(argv, 0, line, "ord");
    if (s->length == 0) {
        aer_error(line, "ord() needs a string with at least one character");
    }
    // read it as unsigned, so characters above 127 come back as 128 to 255 rather than negative
    return aer_int((uint8_t)s->data.chars[0]);
}

// sub(s, start, count): part of a string, count characters from start (0 is the first). Without count, to the end
value_t bi_sub (int argc, value_t* argv, int line) {
    aer_object_t* s = str_arg(argv, 0, line, "sub");
    int32_t start = int_arg(argv, 1, line, "sub");
    int32_t count = argc > 2 ? int_arg(argv, 2, line, "sub") : (int32_t)s->length;
    // out of range numbers are pulled back inside the string rather than reported, so sub()
    // never reads past either end: first start is kept from 0 to the length...
    if (start < 0) {
        start = 0;
    }
    if ((uint32_t)start > s->length) {
        start = (int32_t)s->length;
    }
    // ...then count is kept from 0 to however many characters are left after start
    if (count < 0) {
        count = 0;
    }
    if ((uint32_t)count > s->length - (uint32_t)start) {
        count = (int32_t)(s->length - (uint32_t)start);
    }
    return aer_string(s->data.chars + start, (uint32_t)count);
}

// find(s, part): where part first appears in s (0 is the start), or -1 if it doesn't
value_t bi_find (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* s = str_arg(argv, 0, line, "find");
    aer_object_t* needle = str_arg(argv, 1, line, "find");
    // the position is how far the match is from the start of the string
    char* found = strstr(s->data.chars, needle->data.chars);
    return aer_int(found ? (int32_t)(found - s->data.chars) : -1);
}

// upper() and lower(): a copy of the string argument in capitals (upper true) or small letters.
// fn is the AerLang name, for the error message
static value_t change_case (value_t* argv, int line, const char* fn, bool upper) {
    aer_object_t* s = str_arg(argv, 0, line, fn);
    // copy first and change the copy, the program's own string must stay as it was
    value_t result = aer_string(s->data.chars, s->length);
    // the (unsigned char) casts matter: toupper and tolower expect 0 to 255, and a plain char
    // above 127 would be negative
    for (uint32_t i = 0; i < s->length; i++) {
        char* c = &result.as.obj->data.chars[i];
        *c = (char)(upper ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
    }
    return result;
}

// upper(s): a copy of s in capitals
value_t bi_upper (int argc, value_t* argv, int line) {
    (void)argc;
    return change_case(argv, line, "upper", true);
}

// lower(s): a copy of s in small letters
value_t bi_lower (int argc, value_t* argv, int line) {
    (void)argc;
    return change_case(argv, line, "lower", false);
}

// split(s, sep): s cut into a list of pieces at every sep. split("a,b,c", ",") is ["a", "b", "c"],
// without a separator it splits on spaces
value_t bi_split (int argc, value_t* argv, int line) {
    aer_object_t* s = str_arg(argv, 0, line, "split");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "split")->data.chars : 0;
    size_t sep_length = sep ? slen(sep) : 0;
    // an empty separator would match everywhere without moving on, and never finish
    if (sep && sep_length == 0) {
        aer_error(line, "split() needs a separator that isn't empty");
    }

    // 4 is only a first guess at the size, the list grows as pieces are pushed
    value_t list = aer_list(4);
    const char* p = s->data.chars;
    const char* end = p + s->length;
    // no separator: split into words. Any number of spaces counts as one, and spaces at either
    // end are dropped, so "  a  b " is ["a", "b"] with no empty pieces
    if (!sep) {
        while (p < end) {
            // skip the spaces before the next word
            while (p < end && *p == ' ') {
                p++;
            }
            // then the word runs up to the next space (or the end)
            const char* start = p;
            while (p < end && *p != ' ') {
                p++;
            }
            // nothing between start and p means there were only spaces left
            if (p > start) {
                aer_list_push(list.as.obj, aer_string(start, (uint32_t)(p - start)));
            }
        }
        return list;
    }
    // with a separator every one counts, so "a,,b" is ["a", "", "b"] and "" is [""]
    for (;;) {
        const char* hit = strstr(p, sep);
        // no more separators: whatever is left is the last piece
        if (hit == 0) {
            aer_list_push(list.as.obj, aer_string(p, (uint32_t)(end - p)));
            return list;
        }
        // the piece before the separator, then carry on just after it
        aer_list_push(list.as.obj, aer_string(p, (uint32_t)(hit - p)));
        p = hit + sep_length;
    }
}

// join(list, sep): the items as one string, with sep between them (nothing between them without sep)
value_t bi_join (int argc, value_t* argv, int line) {
    aer_object_t* list = list_arg(argv, 0, line, "join");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "join")->data.chars : "";
    uint32_t sep_length = (uint32_t)slen(sep);

    // turn every item into a string first, so the total length is known
    // (room for at least one, so an empty list never asks aer_alloc for 0 bytes)
    value_t* pieces = aer_alloc((list->length ? list->length : 1) * sizeof(value_t));
    uint32_t total = 0;
    for (uint32_t i = 0; i < list->length; i++) {
        pieces[i] = aer_to_string(list->data.items[i]);
        // a separator goes before every piece but the first
        total += pieces[i].as.obj->length + (i > 0 ? sep_length : 0);
    }
    // start from an empty string and grow it to the full size in one go (+ 1 for the '\0'),
    // rather than growing it again for every piece
    value_t result = aer_string("", 0);
    result.as.obj->data.chars = aer_realloc(result.as.obj->data.chars, total + 1);
    char* out = result.as.obj->data.chars;
    // copy the pieces in, with the separator between them
    for (uint32_t i = 0; i < list->length; i++) {
        if (i > 0) {
            memcpy(out, sep, sep_length);
            out += sep_length;
        }
        memcpy(out, pieces[i].as.obj->data.chars, pieces[i].as.obj->length);
        out += pieces[i].as.obj->length;
        // aer_to_string made each piece a new string that only we hold, so let it go now it is copied
        aer_release(pieces[i]);
    }
    *out = '\0';
    result.as.obj->length = total;
    // the array itself only held the pieces, which have all been released
    aer_free(pieces);
    return result;
}
