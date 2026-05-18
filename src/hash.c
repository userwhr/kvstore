

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "kvstore.h"

#define HASH_LOAD_FACTOR 0.75

// FNV-1a: 优秀的分布性，低冲突
static unsigned int _hash(const char *key, int size) {

	if (!key) return 0;

	unsigned int h = 2166136261u; // FNV offset basis
	const unsigned char *p = (const unsigned char *)key;

	while (*p) {
		h ^= *p++;
		h *= 16777619u; // FNV prime
	}

	// 额外的位混合，减少低位聚集
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;

	return h % size;
}

hashnode_t *_create_node(char *key, char *value) {

	hashnode_t *node = (hashnode_t*)malloc(sizeof(hashnode_t));
	if (!node) return NULL;

#if ENABLE_KEY_POINTER
	char *kcopy = malloc(strlen(key) + 1);
	if (kcopy == NULL) return NULL;
	memset(kcopy, 0, strlen(key) + 1);
	strncpy(kcopy, key, strlen(key));

	node->key = kcopy;

	char *kvalue = malloc(strlen(value) + 1);
	if (kvalue == NULL) {
		free(kcopy);
		return NULL;
	}
	memset(kvalue, 0, strlen(value) + 1);
	strncpy(kvalue, value, strlen(value));

	node->value = kvalue;

#else
	strncpy(node->key, key, MAX_KEY_LEN);
	strncpy(node->value, value, MAX_VALUE_LEN);
#endif
	node->next = NULL;

	return node;
}

// 动态扩容: 负载因子超过阈值时桶数翻倍
static int _hash_resize(kvs_hash_t *hash) {

	int new_size = hash->max_slots * 2;
	hashnode_t **new_nodes = (hashnode_t**)malloc(sizeof(hashnode_t*) * new_size);
	if (!new_nodes) return -1;

	memset(new_nodes, 0, sizeof(hashnode_t*) * new_size);

	// 将所有节点重新哈希到新桶中
	int i = 0;
	for (i = 0; i < hash->max_slots; i++) {
		hashnode_t *node = hash->nodes[i];
		while (node) {
			hashnode_t *next = node->next;
			unsigned int idx = _hash(node->key, new_size);
			node->next = new_nodes[idx];
			new_nodes[idx] = node;
			node = next;
		}
	}

	free(hash->nodes);
	hash->nodes = new_nodes;
	hash->max_slots = new_size;

	return 0;
}

int hash_creat(kvs_hash_t *hash, int size) {

	if (!hash) return -1;
	if (size <= 0) size = DEFAULT_TABLE_SIZE;

	hash->nodes = (hashnode_t**)malloc(sizeof(hashnode_t*) * size);
	if (!hash->nodes) return -1;

	memset(hash->nodes, 0, sizeof(hashnode_t*) * size);
	hash->max_slots = size;
	hash->count = 0;

	return 0;
}

void hash_destory(kvs_hash_t *hash) {

	if (!hash) return;

	int i = 0;
	for (i = 0; i < hash->max_slots; i++) {
		hashnode_t *node = hash->nodes[i];

		while (node != NULL) {

			hashnode_t *tmp = node;
			node = node->next;
#if ENABLE_KEY_POINTER
			free(tmp->key);
			free(tmp->value);
#endif
			free(tmp);
		}
	}

	free(hash->nodes);
}

int hash_set(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key || !value) return -1;

	int idx = _hash(key, hash->max_slots);

	hashnode_t *node = hash->nodes[idx];
	while (node != NULL) {
		if (strcmp(node->key, key) == 0) {
			return 1;
		}
		node = node->next;
	}

	hashnode_t *new_node = _create_node(key, value);
	new_node->next = hash->nodes[idx];
	hash->nodes[idx] = new_node;

	hash->count++;

	// 负载因子检查，触发扩容
	if ((double)hash->count / hash->max_slots > HASH_LOAD_FACTOR) {
		_hash_resize(hash);
	}

	return 0;
}

char * hash_get(kvs_hash_t *hash, char *key) {

	if (!hash || !key) return NULL;

	int idx = _hash(key, hash->max_slots);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			return node->value;
		}

		node = node->next;
	}

	return NULL;
}

int hash_mod(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key) return -1;

	int idx = _hash(key, hash->max_slots);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			break;
		}

		node = node->next;
	}

	if (node == NULL) {
		return 1;
	}

	// node -->
	free(node->value);

	char *kvalue = malloc(strlen(value) + 1);
	if (kvalue == NULL) return -2;
	memset(kvalue, 0, strlen(value) + 1);
	strncpy(kvalue, value, strlen(value));
	node->value = kvalue;

	return 0;
}

int hash_count(kvs_hash_t *hash) {
	return hash->count;
}

int hash_del(kvs_hash_t *hash, char *key) {
	if (!hash || !key) return -2;

	int idx = _hash(key, hash->max_slots);

	hashnode_t *head = hash->nodes[idx];
	if (head == NULL) return -1; // noexist

	// head node
	if (strcmp(head->key, key) == 0) {
		hashnode_t *tmp = head->next;
		hash->nodes[idx] = tmp;
#if ENABLE_KEY_POINTER
		free(head->key);
		free(head->value);
#endif
		free(head);
		hash->count--;

		return 0;
	}

	hashnode_t *cur = head;
	while (cur->next != NULL) {
		if (strcmp(cur->next->key, key) == 0) break;

		cur = cur->next;
	}

	if (cur->next == NULL) {
		return -1;
	}

	hashnode_t *tmp = cur->next;
	cur->next = tmp->next;
#if ENABLE_KEY_POINTER
	free(tmp->key);
	free(tmp->value);
#endif
	free(tmp);

	hash->count--;

	return 0;
}

int hash_exist(kvs_hash_t *hash, char *key) {

	char *value = hash_get(hash, key);
	if (!value) return 1;

	return 0;
}

