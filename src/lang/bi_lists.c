/*
    bi_lists.c
    AerLang's built-in functions for lists (list, push, pop, insert, remove) and bytes, the
    buffers that hold binary data such as pictures and sounds. (len() works on both, and is
    with the strings in bi_text.c; join() and split() are there too.)

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../lib/aer_string.h"

// list(n, fill): a new list of n items, each one fill (nil without it)
value_t bi_list (int argc, value_t* argv, int line) {
    int32_t count = int_arg(argv, 0, line, "list");
    // a limit, so a mistake like list(-1) or a huge number can't take all the memory at once
    if (count < 0 || count > 1000000) {
        aer_error(line, "list() size must be 0 to 1000000");
    }
    value_t fill = argc > 1 ? argv[1] : aer_nil();
    // made with room for every item, so the pushes below never have to grow it
    value_t list = aer_list((uint32_t)count);
    // fill is borrowed from the caller, and aer_list_push takes over a reference, so every slot
    // needs a reference of its own: one aer_retain per item. (All the slots share the same
    // value, so list(3, []) is three names for one list, not three lists)
    for (int32_t i = 0; i < count; i++) {
        aer_retain(fill);
        aer_list_push(list.as.obj, fill);
    }
    return list;
}

// push(list, x): add x to the end of a list
value_t bi_push (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "push");
    // the caller still owns argv[1] and the list takes over a reference, so the list needs one of its own
    aer_retain(argv[1]);
    aer_list_push(list, argv[1]);
    return aer_nil();
}

// pop(list): take the last item off a list and give it back (nil if the list is empty)
value_t bi_pop (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "pop");
    if (list->length == 0) {
        return aer_nil();
    }
    return list->data.items[--list->length]; // the list's reference passes to the caller
}

// insert(list, at, x): put x into a list at position at, moving the items after it along one
value_t bi_insert (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "insert");
    int32_t at = int_arg(argv, 1, line, "insert");
    // at may equal the length: that puts x on the end, the same as push()
    if (at < 0 || (uint32_t)at > list->length) {
        aer_error(line, "insert() position %d is out of range", at);
    }
    // the list keeps its own reference to x, as in push()
    aer_retain(argv[2]);
    aer_list_push(list, aer_nil()); // make room
    // slide the items from at onwards up one place (memmove, as the two ranges overlap).
    // length already counts the new slot, so length - 1 - at items move
    memmove(&list->data.items[at + 1], &list->data.items[at], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->data.items[at] = argv[2];
    return aer_nil();
}

// remove(list, at): take the item at position at out of a list, and give it back
value_t bi_remove (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "remove");
    int32_t at = int_arg(argv, 1, line, "remove");
    if (at < 0 || (uint32_t)at >= list->length) {
        aer_error(line, "remove() position %d is out of range", at);
    }
    // take the item out, then slide the ones after it down one place to close the gap
    value_t item = list->data.items[at];
    memmove(&list->data.items[at], &list->data.items[at + 1], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->length--;
    // no retain or release: the list's reference to the item passes to the caller
    return item;
}

// bytes(n): n bytes, all 0, for binary data such as pictures and sounds
value_t bi_bytes (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t length = int_arg(argv, 0, line, "bytes");
    // 64MB is far more than a picture or a sound needs, and stops a mistake taking all the memory
    if (length < 0 || length > 64 * 1024 * 1024) {
        aer_error(line, "bytes() size must be 0 to 64MB");
    }
    return aer_bytes((uint32_t)length);
}
