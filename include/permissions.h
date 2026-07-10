/*
 * permissions.h - unix-style rwx on key namespaces (prefix before ':').
 * each namespace = owner + group + 3-octal-digit mode, exactly like a file:
 * r=4 w=2 x=1 per triad. every read/write gets checked.
 */
#ifndef DURAKV_PERMISSIONS_H
#define DURAKV_PERMISSIONS_H

typedef struct PermTable PermTable;

PermTable *perm_create(void);
void       perm_destroy(PermTable *p);

/* Define (or update) a namespace's owner, group and mode (e.g. 0640). */
int perm_set(PermTable *p, const char *ns, const char *owner,
             const char *group, int mode);

/* May `user` (in `user_group`) perform op on `ns`? op is 'r', 'w', or 'x'.
 * Returns 1 if allowed, 0 if denied (unknown namespace => denied). */
int perm_check(PermTable *p, const char *ns, const char *user,
               const char *user_group, char op);

#endif /* DURAKV_PERMISSIONS_H */
