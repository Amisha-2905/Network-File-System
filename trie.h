#ifndef TRIE_H
#define TRIE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHILDREN 128

typedef struct TrieNode
{
    char name[256];
    int ss_id; // Storage Server index matching ss_table array (-1 if directory/non-terminal)
    struct TrieNode *children[MAX_CHILDREN];
    int child_count;
} TrieNode;

// Allocate a clean memory leaf instance
static inline TrieNode *create_trie_node(const char *name)
{
    TrieNode *node = (TrieNode *)malloc(sizeof(TrieNode));
    if (node)
    {
        strncpy(node->name, name, 256);
        node->ss_id = -1;
        node->child_count = 0;
        memset(node->children, 0, sizeof(node->children));
    }
    return node;
}

// Tokenize paths and insert them structurally into the hierarchical Trie
static inline void trie_insert(TrieNode *root, const char *path, int ss_id)
{
    char path_copy[512];
    strncpy(path_copy, path, sizeof(path_copy));

    char *token = strtok(path_copy, "/");
    TrieNode *current = root;

    while (token != NULL)
    {
        int found = -1;
        for (int i = 0; i < current->child_count; i++)
        {
            if (strcmp(current->children[i]->name, token) == 0)
            {
                found = i;
                break;
            }
        }

        if (found == -1)
        {
            if (current->child_count < MAX_CHILDREN)
            {
                TrieNode *new_node = create_trie_node(token);
                current->children[current->child_count] = new_node;
                found = current->child_count;
                current->child_count++;
            }
            else
            {
                return; // Guard overflow limits
            }
        }
        current = current->children[found];
        token = strtok(NULL, "/");
    }
    current->ss_id = ss_id; // Leaf node records owning storage server identifier index
}

// Rapid structural traversal searching
static inline int trie_search(TrieNode *root, const char *path)
{
    char path_copy[512];
    strncpy(path_copy, path, sizeof(path_copy));

    char *token = strtok(path_copy, "/");
    TrieNode *current = root;

    while (token != NULL)
    {
        int found = -1;
        for (int i = 0; i < current->child_count; i++)
        {
            if (strcmp(current->children[i]->name, token) == 0)
            {
                found = i;
                break;
            }
        }
        if (found == -1)
            return -1; // Missing segment break context
        current = current->children[found];
        token = strtok(NULL, "/");
    }
    return current->ss_id;
}

// Remove leaf links or wipe paths during cluster deletions
static inline void trie_delete_path(TrieNode *root, const char *path)
{
    char path_copy[512];
    strncpy(path_copy, path, sizeof(path_copy));

    char *token = strtok(path_copy, "/");
    TrieNode *current = root;

    while (token != NULL)
    {
        int found = -1;
        for (int i = 0; i < current->child_count; i++)
        {
            if (strcmp(current->children[i]->name, token) == 0)
            {
                found = i;
                break;
            }
        }
        if (found == -1)
            return;
        current = current->children[found];
        token = strtok(NULL, "/");
    }
    current->ss_id = -1; // Clear ownership binding identifier without destroying structural parent branches
}

#endif