#include "structs.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Allocates and initializes a new TrieNode.
 * Returns a pointer to the newly created TrieNode.
 */
struct TrieNode *getNode(void) {
    // Allocate memory for a new TrieNode and zero-initialize it
    struct TrieNode *new_node = calloc(1, sizeof(struct TrieNode));
    if (new_node == NULL) {
        fprintf(stderr, "getNode: Memory allocation failed.\n");
        return NULL;
    }

    // Initialize isEndOfWord to false and ss_ptr to NULL
    new_node->isEndOfWord = false;
    new_node->ss_ptr = NULL;

    // Although calloc initializes all children to NULL, explicitly setting them for clarity
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        new_node->children[i] = NULL;
    }

    return new_node;
}

/**
 * Counts the number of occurrences of a specific character in a string.
 * Returns the total count.
 */
int countCharacter(const char *str, char target) {
    if (str == NULL) {
        return 0;
    }

    int total = 0;
    while (*str) {
        if (*str++ == target) {
            total++;
        }
    }
    return total;
}

/**
 * Inserts a key into the Trie and associates it with an ss_info pointer.
 */
void insert(struct TrieNode *root, const char *key, ss_info *ptr) {
    if (root == NULL || key == NULL) {
        fprintf(stderr, "insert: Invalid arguments provided.\n");
        return;
    }

    struct TrieNode *current = root;
    size_t key_length = strlen(key);

    for (size_t level = 0; level < key_length; level++) {
        unsigned char current_char = (unsigned char)key[level];
        // Ensure the character is within the valid range
        if (current_char >= ALPHABET_SIZE) {
            fprintf(stderr, "insert: Character '%c' out of bounds in key '%s'.\n", current_char, key);
            return;
        }

        // If the child node doesn't exist, create it
        if (current->children[current_char] == NULL) {
            current->children[current_char] = getNode();
            if (current->children[current_char] == NULL) {
                fprintf(stderr, "insert: Failed to create child node for character '%c'.\n", current_char);
                return;
            }
        }

        // Move to the child node
        current = current->children[current_char];
    }

    // Mark the end of the word and assign the ss_info pointer
    current->isEndOfWord = true;
    current->ss_ptr = ptr;
}

/**
 * Searches for a key in the Trie and returns the associated ss_info pointer.
 * Returns NULL if the key is not found.
 */
ss_info *search(struct TrieNode *root, char *key) {
    if (root == NULL || key == NULL) {
        fprintf(stderr, "search: Invalid arguments provided.\n");
        return NULL;
    }

    struct TrieNode *current = root;
    size_t key_length = strlen(key);
    bool has_dot = strchr(key, '.') ? true : false;
    int slash_count = countCharacter(key, '/');

    // If the key starts with a dot, skip it
    if (key[0] == '.') {
        key++;
        key_length--;
    }

    for (size_t level = 0; level < key_length; level++) {
        unsigned char current_char = (unsigned char)key[level];

        // If the character is a slash, handle accordingly
        if (current_char == '/') {
            if (slash_count == 1 && has_dot) {
                if (current->children[current_char] && current->children[current_char]->ss_ptr) {
                    return current->children[current_char]->ss_ptr;
                } else {
                    return NULL;
                }
            }
            slash_count--;
        }

        // If the child node doesn't exist, the key isn't present
        if (current_char >= ALPHABET_SIZE || current->children[current_char] == NULL) {
            return NULL;
        }

        // Move to the child node
        current = current->children[current_char];
    }

    // If the end of the key is reached, return the associated ss_info pointer
    if (current->isEndOfWord) {
        return current->ss_ptr;
    }

    return NULL;
}
