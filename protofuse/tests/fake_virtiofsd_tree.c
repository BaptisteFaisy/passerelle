/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/fake_virtiofsd_tree.c - Simulated directory tree of the fake
 * virtiofs server.
 *
 * Implements requirements:
 *   - VFS-03: nodes, names and file contents for the in-memory server
 *   - VFS-04: node identifiers are what the client tracks with references
 */
#include "fake_virtiofsd.h"

#include <stdlib.h>
#include <string.h>

static fv_node *node_new(uint64_t id, const char *name, bool is_dir)
{
    fv_node *n = (fv_node *)calloc(1, sizeof(*n));
    if (n == NULL)
        return NULL;
    n->nodeid = id;
    n->is_dir = is_dir;
    n->mode = is_dir ? 0755u : 0644u;
    if (name != NULL) {
        size_t len = strlen(name);
        if (len > FV_NAME_MAX)
            len = FV_NAME_MAX;
        memcpy(n->name, name, len);
        n->name[len] = '\0';
    }
    return n;
}

static void node_free(fv_node *n)
{
    while (n != NULL) {
        fv_node *next = n->sibling;
        node_free(n->child);
        free(n->data);
        free(n);
        n = next;
    }
}

fake_virtiofsd *fv_new(uint32_t minor, uint32_t max_write)
{
    fake_virtiofsd *f = (fake_virtiofsd *)calloc(1, sizeof(*f));
    if (f == NULL)
        return NULL;
    f->major = PGW_FUSE_MAJOR;
    f->minor = minor;
    f->max_write = (max_write == 0) ? 65536u : max_write;
    f->max_pages = 16;
    f->flags = PGW_FUSE_F_ASYNC_READ | PGW_FUSE_F_BIG_WRITES;
    f->next_nodeid = PGW_FUSE_ROOT_ID;
    f->next_fh = 100;
    f->root = node_new(PGW_FUSE_ROOT_ID, "", true);
    if (f->root == NULL) {
        free(f);
        return NULL;
    }
    f->root->parent = NULL;
    return f;
}

void fv_free(fake_virtiofsd *f)
{
    if (f == NULL)
        return;
    node_free(f->root);
    free(f);
}

fv_node *fv_node_by_id(fake_virtiofsd *f, uint64_t nodeid)
{
    if (f == NULL)
        return NULL;
    if (nodeid == PGW_FUSE_ROOT_ID)
        return f->root;
    fv_node *stack[64];
    size_t sp = 0;
    if (f->root != NULL)
        stack[sp++] = f->root;
    while (sp > 0) {
        fv_node *n = stack[--sp];
        for (fv_node *c = n->child; c != NULL; c = c->sibling) {
            if (c->nodeid == nodeid)
                return c;
            if (c->child != NULL && sp < 64u)
                stack[sp++] = c;
        }
    }
    return NULL;
}

fv_node *fv_child(fake_virtiofsd *f, uint64_t parent, const char *name)
{
    fv_node *p = fv_node_by_id(f, parent);
    if (p == NULL || name == NULL)
        return NULL;
    for (fv_node *c = p->child; c != NULL; c = c->sibling) {
        if (strcmp(c->name, name) == 0)
            return c;
    }
    return NULL;
}

size_t fv_node_count(const fake_virtiofsd *f)
{
    if (f == NULL)
        return 0;
    size_t total = 0;
    const fv_node *stack[64];
    size_t sp = 0;
    if (f->root != NULL)
        stack[sp++] = f->root;
    while (sp > 0) {
        const fv_node *n = stack[--sp];
        total++;
        for (const fv_node *c = n->child; c != NULL; c = c->sibling) {
            if (c->child != NULL && sp < 64u)
                stack[sp++] = c;
        }
    }
    return total;
}

/* Create a child of `parent`; returns NULL when the name is taken or invalid. */
fv_node *fv_add(fake_virtiofsd *f, fv_node *parent, const char *name, bool is_dir)
{
    if (f == NULL || parent == NULL || name == NULL || name[0] == '\0')
        return NULL;
    if (!parent->is_dir || strlen(name) > FV_NAME_MAX)
        return NULL;
    for (fv_node *c = parent->child; c != NULL; c = c->sibling) {
        if (strcmp(c->name, name) == 0)
            return NULL;
    }
    fv_node *n = node_new(++f->next_nodeid, name, is_dir);
    if (n == NULL)
        return NULL;
    n->parent = parent;
    n->sibling = parent->child;
    parent->child = n;
    return n;
}

/* Detach a child from its parent without freeing it (RENAME moves it). */
fv_node *fv_detach(fv_node *parent, const char *name)
{
    if (parent == NULL || name == NULL)
        return NULL;
    fv_node **link = &parent->child;
    while (*link != NULL) {
        if (strcmp((*link)->name, name) == 0) {
            fv_node *n = *link;
            *link = n->sibling;
            n->sibling = NULL;
            return n;
        }
        link = &(*link)->sibling;
    }
    return NULL;
}

bool fv_remove(fake_virtiofsd *f, fv_node *parent, const char *name)
{
    if (f == NULL || parent == NULL || name == NULL)
        return false;
    fv_node **link = &parent->child;
    while (*link != NULL) {
        if (strcmp((*link)->name, name) == 0) {
            fv_node *dead = *link;
            *link = dead->sibling;
            dead->sibling = NULL;
            node_free(dead);
            return true;
        }
        link = &(*link)->sibling;
    }
    return false;
}

/* Resize the content buffer of a regular file, zero-filling the new bytes. */
bool fv_set_size(fv_node *n, uint64_t size)
{
    if (n == NULL || n->is_dir || n->is_link)
        return false;
    if (size > 1024u * 1024u)
        return false;
    if ((size_t)size > n->cap) {
        size_t cap = (n->cap == 0) ? 256u : n->cap;
        while (cap < (size_t)size)
            cap *= 2u;
        uint8_t *nd = (uint8_t *)realloc(n->data, cap);
        if (nd == NULL)
            return false;
        memset(nd + n->cap, 0, cap - n->cap);
        n->data = nd;
        n->cap = cap;
    }
    n->size = size;
    return true;
}

uint64_t fv_path_id(fake_virtiofsd *f, const char *path)
{
    if (f == NULL || path == NULL || f->root == NULL)
        return 0;
    uint64_t cur = f->root->nodeid;
    const char *p = path;
    while (*p == '/')
        p++;
    char part[FV_NAME_MAX + 1u];
    while (*p != '\0') {
        size_t len = 0;
        while (p[len] != '\0' && p[len] != '/' && len < FV_NAME_MAX)
            len++;
        memcpy(part, p, len);
        part[len] = '\0';
        fv_node *c = fv_child(f, cur, part);
        if (c == NULL)
            return 0;
        cur = c->nodeid;
        p += len;
        while (*p == '/')
            p++;
    }
    return cur;
}
