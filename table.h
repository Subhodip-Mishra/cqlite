#ifndef TABLE_H
#define TABLE_H

#include "pager.h"
#include "wal.h"
#include <stdint.h>
#include <stdbool.h>

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE    255

typedef struct Row {
    uint32_t id;
    char username[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
} Row;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

#define ID_SIZE         size_of_attribute(Row, id)
#define USERNAME_SIZE   size_of_attribute(Row, username)
#define EMAIL_SIZE      size_of_attribute(Row, email)

#define ID_OFFSET       0
#define USERNAME_OFFSET (ID_OFFSET + ID_SIZE)
#define EMAIL_OFFSET    (USERNAME_OFFSET + USERNAME_SIZE)
#define ROW_SIZE        (ID_SIZE + USERNAME_SIZE + EMAIL_SIZE)

typedef struct Table {
    Pager* pager;
    uint32_t root_page_num;
    Wal* wal;
    bool in_transaction;
} Table;

void serialize_row(Row* source, void* destination);
void deserialize_row(void* source, Row* destination);
Table* db_open(const char* filename);
void db_close(Table* table);
void print_row(Row* row);

#endif // TABLE_H
