/*
 * auth.h - user store + login. only the argon2id hash is kept, never the
 * password. login ok -> session id > 0; server rejects data commands from
 * connections that havent logged in.
 */
#ifndef DURAKV_AUTH_H
#define DURAKV_AUTH_H

typedef struct AuthStore AuthStore;

AuthStore *auth_create(void);
void       auth_destroy(AuthStore *s);

/* Register a user. Returns 0 on success, -1 if the user already exists. */
int auth_register(AuthStore *s, const char *user, const char *password,
                  const char *group);

/* Verify credentials. Returns a session id (> 0) on success, or 0 on failure. */
int auth_login(AuthStore *s, const char *user, const char *password);

/* The group a user belongs to, or NULL if unknown. */
const char *auth_group_of(AuthStore *s, const char *user);

#endif /* DURAKV_AUTH_H */
