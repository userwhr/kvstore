#ifndef _HASH_H
#define _HASH_H
#define MAX_KEY_LEN 128
#define MAX_VALUE_LEN 512
#define MAX_TABLE_SIZE 1024
// hash
typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
  char *key;
  char *value;
#else
  char key[MAX_KEY_LEN];
  char value[MAX_VALUE_LEN];
#endif
  struct hashnode_s *next;

} hashnode_t;

typedef struct hashtable_s {

  hashnode_t **nodes; //* change **,

  int max_slots;
  int count;

} hashtable_t;

typedef struct hashtable_s kvs_hash_t;

int kvs_hash_creat(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(hashtable_t *hash, char *key, char *value);
char *kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);
// end hash
#endif