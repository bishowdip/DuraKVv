/*
 * auth.c - register users, verify logins, hand out session ids. in-memory
 * store, each user has a group (feeds the rwx model in permissions.c).
 * passwords are never stored or compared in plaintext -- only the argon2id
 * hash, and verify is constant time.
 */
#include "auth.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char user[64];
    char group[64];
    char hash[CRYPTO_PWHASH_STRBYTES];   /* Argon2id hash string (never plaintext) */
} User;

struct AuthStore {
    User  *users;
    size_t n, cap;
    int    next_session;                 /* monotonic id handed out on login */
};

AuthStore *auth_create(void)
{
    if (crypto_init() != 0) return NULL;
    AuthStore *s = calloc(1, sizeof(*s));
    s->next_session = 1;
    return s;
}

void auth_destroy(AuthStore *s)
{
    if (!s) return;
    free(s->users);
    free(s);
}

static User *find_user(AuthStore *s, const char *user)
{
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->users[i].user, user) == 0) return &s->users[i];
    return NULL;
}

/* add a user (password hashed before storing). -1 if name taken. */
int auth_register(AuthStore *s, const char *user, const char *password,
                  const char *group)
{
    if (find_user(s, user)) return -1;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->users = realloc(s->users, s->cap * sizeof(User));
    }
    User *u = &s->users[s->n];
    snprintf(u->user, sizeof(u->user), "%s", user);
    snprintf(u->group, sizeof(u->group), "%s", group);
    if (crypto_password_hash(u->hash, password) != 0) return -1;
    s->n++;
    return 0;
}

/* login. session id > 0 on success, 0 on failure. unknown user and wrong
 * password both return the same 0 on purpose -- dont leak which it was. */
int auth_login(AuthStore *s, const char *user, const char *password)
{
    User *u = find_user(s, user);
    if (!u) return 0;                                  /* unknown user */
    if (crypto_password_verify(u->hash, password) != 0) return 0;  /* bad pass */
    return s->next_session++;                          /* issue a session */
}

const char *auth_group_of(AuthStore *s, const char *user)
{
    User *u = find_user(s, user);
    return u ? u->group : NULL;
}
