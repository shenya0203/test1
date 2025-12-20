#ifndef UCI_H
#define UCI_H

#include <stdint.h>
#include <stdlib.h>

// UCI error codes
#define UCI_OK 0
#define UCI_ERR_MEM -1
#define UCI_ERR_INVAL -2
#define UCI_ERR_NOTFOUND -3
#define UCI_ERR_IO -4
#define UCI_ERR_PARSE -5

// UCI option types
enum uci_option_type {
    UCI_TYPE_STRING = 0,
    UCI_TYPE_LIST = 1,
};

// Forward declarations
struct uci_context;
struct uci_package;
struct uci_section;
struct uci_option;
struct uci_element;
struct uci_list;

// UCI context functions
struct uci_context *uci_alloc_context(void);
void uci_free_context(struct uci_context *ctx);

// UCI package functions
int uci_load(struct uci_context *ctx, const char *config, struct uci_package **package);
void uci_unload(struct uci_context *ctx, struct uci_package *p);

// UCI element/section functions
#define uci_foreach_element(list, element) \
    for (element = (list)->next; element != (void *)(list); element = element->next)

struct uci_section *uci_to_section(struct uci_element *e);

// UCI option functions
struct uci_option *uci_lookup_option(struct uci_context *ctx, struct uci_section *s, const char *name);

// Stub implementations for missing UCI functions
// TODO: Replace with proper UCI library implementations
inline struct uci_context *uci_alloc_context(void) { return NULL; }
inline void uci_free_context(struct uci_context *ctx) { (void)ctx; }
inline int uci_load(struct uci_context *ctx, const char *config, struct uci_package **package) {
    (void)ctx; (void)config; (void)package; return -1;
}
inline void uci_unload(struct uci_context *ctx, struct uci_package *p) { (void)ctx; (void)p; }
inline struct uci_section *uci_to_section(struct uci_element *e) { return (struct uci_section *)e; }
inline struct uci_option *uci_lookup_option(struct uci_context *ctx, struct uci_section *s, const char *name) {
    (void)ctx; (void)s; (void)name; return NULL;
}

// UCI structures
struct uci_list {
    struct uci_element *next;
    struct uci_element *prev;
};

struct uci_element {
    struct uci_list list;
    char *name;
    struct uci_element *next;
};

struct uci_package {
    struct uci_list sections;
    // ... other fields
};

struct uci_section {
    struct uci_element e;
    struct uci_list options;
    // ... other fields
};

struct uci_option {
    struct uci_element e;
    enum uci_option_type type;
    union {
        char *string;
        struct uci_list list;
    } v;
};

#endif /* UCI_H */
