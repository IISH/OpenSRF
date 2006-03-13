#ifndef OSRF_HASH_H
#define OSRF_HASH_H

#include "opensrf/utils.h"
#include "opensrf/string_array.h"
#include "osrf_list.h"

#define OSRF_HASH_MAXKEY 256

#define OSRF_HASH_KEY_MASK 0x7FF  /* hash keys are a maximun of 2047 in size */
#define OSRF_HASH_KEY_SIZE 2048  /* size of the container list for the keys */


struct __osrfHashStruct {
	osrfList* hash; /* this hash */
	void (*freeItem) (char* key, void* item);	/* callback for freeing stored items */
	unsigned int size;
};
typedef struct __osrfHashStruct osrfHash;

struct _osrfHashNodeStruct {
	char* key;
	void* item;
};
typedef struct _osrfHashNodeStruct osrfHashNode;


struct __osrfHashIteratorStruct {
	char* current;
	int currentIdx;
	osrfHash* hash;
	osrfStringArray* keys;
};
typedef struct __osrfHashIteratorStruct osrfHashIterator;

osrfHashNode* osrfNewHashNode(char* key, void* item);
void osrfHashNodeFree(osrfHashNode*);

/**
  Allocates a new hash object
  */
osrfHash* osrfNewHash();

/**
  Sets the given key with the given item
  if "freeItem" is defined and an item already exists at the given location, 
  then old item is freed and the new item is put into place.
  if "freeItem" is not defined and an item already exists, the old item
  is returned.
  @return The old item if exists and there is no 'freeItem', returns NULL
  otherwise
  */
void* osrfHashSet( osrfHash* hash, void* item, const char* key, ... );

/**
  Removes an item from the hash.
  if 'freeItem' is defined it is used and NULL is returned,
  else the freed item is returned
  */
void* osrfHashRemove( osrfHash* hash, const char* key, ... );

void* osrfHashGet( osrfHash* hash, const char* key, ... );


/**
  @return A list of strings representing the keys of the hash. 
  caller is responsible for freeing the returned string array 
  with osrfStringArrayFree();
  */
osrfStringArray* osrfHashKeys( osrfHash* hash );

/**
  Frees a hash
  */
void osrfHashFree( osrfHash* hash );

/**
  @return The number of items in the hash
  */
unsigned long osrfHashGetCount( osrfHash* hash );




/**
  Creates a new list iterator with the given list
  */
osrfHashIterator* osrfNewHashIterator( osrfHash* hash );

/**
  Returns the next non-NULL item in the list, return NULL when
  the end of the list has been reached
  */
void* osrfHashIteratorNext( osrfHashIterator* itr );

/**
  Deallocates the given list
  */
void osrfHashIteratorFree( osrfHashIterator* itr );

void osrfHashIteratorReset( osrfHashIterator* itr );

#endif
