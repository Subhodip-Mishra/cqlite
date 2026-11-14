#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "table.h"
#include "btree.h"

typedef struct
{
    char *buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

typedef enum
{
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum
{
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_STRING_TOO_LONG,
    PREPARE_NEGATIVE_ID,
    PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum
{
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_NOT_FOUND,
    EXECUTE_TABLE_FULL
} ExecuteResult;

typedef enum
{
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_UPDATE,
    STATEMENT_DELETE
} StatementType;

typedef struct
{
    StatementType type;
    Row row_to_insert;
    bool has_where_id;
    uint32_t target_id;
} Statement;

InputBuffer *new_input_buffer(void)
{
    InputBuffer *input_buffer = malloc(sizeof(InputBuffer));
    if (input_buffer == NULL)
    {
        printf("Memory allocation failed for InputBuffer\n");
        exit(EXIT_FAILURE);
    }
    input_buffer->buffer = NULL;
    input_buffer->buffer_length = 0;
    input_buffer->input_length = 0;
    return input_buffer;
}

void close_input_buffer(InputBuffer *input_buffer)
{
    free(input_buffer->buffer);
    free(input_buffer);
}

void print_prompt(void)
{
    printf("cqlite > ");
}

void read_input(InputBuffer *input_buffer)
{
    ssize_t bytes_read = getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

    if (bytes_read <= 0)
    {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }

    input_buffer->input_length = bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = 0;
}

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table)
{
    if (strcmp(input_buffer->buffer, ".exit") == 0)
    {
        close_input_buffer(input_buffer);
        db_close(table);
        exit(EXIT_SUCCESS);
    }
    if (strcmp(input_buffer->buffer, ".btree") == 0)
    {
        printf("Tree:\n");
        print_tree(table->pager, 0, 0);
        return META_COMMAND_SUCCESS;
    }
    if (strcmp(input_buffer->buffer, ".stats") == 0)
    {
        printf("Database Stats:\n");
        printf("  Page Size:        %d bytes\n", PAGE_SIZE);
        printf("  Row Size:         %lu bytes\n", (unsigned long)ROW_SIZE);
        printf("  Leaf Max Cells:   %lu\n", (unsigned long)LEAF_NODE_MAX_CELLS);
        printf("  Allocated Pages:  %u\n", table->pager->num_pages);
        printf("  Cached Pages:     %u / %d (Hits: %lu, Misses: %lu)\n",
               table->pager->cached_pages, CACHE_MAX_PAGES,
               table->pager->cache_hits, table->pager->cache_misses);
        return META_COMMAND_SUCCESS;
    }
    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

static bool parse_uint32(const char *str, uint32_t *out, bool *is_negative)
{
    if (str == NULL || *str == '\0')
    {
        return false;
    }
    if (*str == '-')
    {
        *is_negative = true;
        return false;
    }
    *is_negative = false;
    char *endptr = NULL;
    errno = 0;
    unsigned long val = strtoul(str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || val > UINT32_MAX)
    {
        return false;
    }
    *out = (uint32_t)val;
    return true;
}

PrepareResult prepare_insert(InputBuffer *input_buffer, Statement *statement)
{
    statement->type = STATEMENT_INSERT;

    strtok(input_buffer->buffer, " ");
    char *id_string = strtok(NULL, " ");
    char *username = strtok(NULL, " ");
    char *email = strtok(NULL, " ");

    if (id_string == NULL || username == NULL || email == NULL || strtok(NULL, " ") != NULL)
    {
        return PREPARE_SYNTAX_ERROR;
    }

    uint32_t id = 0;
    bool is_negative = false;
    if (!parse_uint32(id_string, &id, &is_negative))
    {
        return is_negative ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
    }
    if (strlen(username) > COLUMN_USERNAME_SIZE || strlen(email) > COLUMN_EMAIL_SIZE)
    {
        return PREPARE_STRING_TOO_LONG;
    }

    statement->row_to_insert.id = id;
    strcpy(statement->row_to_insert.username, username);
    strcpy(statement->row_to_insert.email, email);

    return PREPARE_SUCCESS;
}

PrepareResult prepare_select(InputBuffer *input_buffer, Statement *statement)
{
    statement->type = STATEMENT_SELECT;
    statement->has_where_id = false;
    statement->target_id = 0;

    strtok(input_buffer->buffer, " ");
    char *arg1 = strtok(NULL, " ");

    if (arg1 == NULL)
    {
        return PREPARE_SUCCESS;
    }

    char *id_string = NULL;
    if (strcmp(arg1, "where") == 0)
    {
        char *col = strtok(NULL, " ");
        char *eq = strtok(NULL, " ");
        id_string = strtok(NULL, " ");
        if (col == NULL || strcmp(col, "id") != 0 || eq == NULL || strcmp(eq, "=") != 0 || id_string == NULL)
        {
            return PREPARE_SYNTAX_ERROR;
        }
    }
    else
    {
        id_string = arg1;
    }

    if (strtok(NULL, " ") != NULL)
    {
        return PREPARE_SYNTAX_ERROR;
    }

    uint32_t id = 0;
    bool is_negative = false;
    if (!parse_uint32(id_string, &id, &is_negative))
    {
        return is_negative ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
    }

    statement->has_where_id = true;
    statement->target_id = id;
    return PREPARE_SUCCESS;
}

PrepareResult prepare_update(InputBuffer *input_buffer, Statement *statement)
{
    statement->type = STATEMENT_UPDATE;

    strtok(input_buffer->buffer, " ");
    char *id_string = strtok(NULL, " ");
    char *username = strtok(NULL, " ");
    char *email = strtok(NULL, " ");

    if (id_string == NULL || username == NULL || email == NULL || strtok(NULL, " ") != NULL)
    {
        return PREPARE_SYNTAX_ERROR;
    }

    uint32_t id = 0;
    bool is_negative = false;
    if (!parse_uint32(id_string, &id, &is_negative))
    {
        return is_negative ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
    }
    if (strlen(username) > COLUMN_USERNAME_SIZE || strlen(email) > COLUMN_EMAIL_SIZE)
    {
        return PREPARE_STRING_TOO_LONG;
    }

    statement->row_to_insert.id = id;
    strcpy(statement->row_to_insert.username, username);
    strcpy(statement->row_to_insert.email, email);

    return PREPARE_SUCCESS;
}

PrepareResult prepare_delete(InputBuffer *input_buffer, Statement *statement)
{
    statement->type = STATEMENT_DELETE;
    statement->has_where_id = true;

    strtok(input_buffer->buffer, " ");
    char *arg1 = strtok(NULL, " ");
    if (arg1 == NULL)
    {
        return PREPARE_SYNTAX_ERROR;
    }

    char *id_string = NULL;
    if (strcmp(arg1, "where") == 0)
    {
        char *col = strtok(NULL, " ");
        char *eq = strtok(NULL, " ");
        id_string = strtok(NULL, " ");
        if (col == NULL || strcmp(col, "id") != 0 || eq == NULL || strcmp(eq, "=") != 0 || id_string == NULL)
        {
            return PREPARE_SYNTAX_ERROR;
        }
    }
    else
    {
        id_string = arg1;
    }

    if (strtok(NULL, " ") != NULL)
    {
        return PREPARE_SYNTAX_ERROR;
    }

    uint32_t id = 0;
    bool is_negative = false;
    if (!parse_uint32(id_string, &id, &is_negative))
    {
        return is_negative ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
    }

    statement->target_id = id;
    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer *input_buffer, Statement *statement)
{
    if (strncmp(input_buffer->buffer, "insert", 6) == 0 &&
        (input_buffer->buffer[6] == ' ' || input_buffer->buffer[6] == '\0'))
    {
        return prepare_insert(input_buffer, statement);
    }
    if (strncmp(input_buffer->buffer, "select", 6) == 0 &&
        (input_buffer->buffer[6] == ' ' || input_buffer->buffer[6] == '\0'))
    {
        return prepare_select(input_buffer, statement);
    }
    if (strncmp(input_buffer->buffer, "update", 6) == 0 &&
        (input_buffer->buffer[6] == ' ' || input_buffer->buffer[6] == '\0'))
    {
        return prepare_update(input_buffer, statement);
    }
    if (strncmp(input_buffer->buffer, "delete", 6) == 0 &&
        (input_buffer->buffer[6] == ' ' || input_buffer->buffer[6] == '\0'))
    {
        return prepare_delete(input_buffer, statement);
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}

ExecuteResult execute_insert(Statement *statement, Table *table)
{
    Row *row_to_insert = &(statement->row_to_insert);
    uint32_t key_to_insert = row_to_insert->id;
    Cursor *cursor = table_find(table, key_to_insert);

    void *node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = (*leaf_node_num_cells(node));

    if (cursor->cell_num < num_cells)
    {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
        if (key_at_index == key_to_insert)
        {
            free(cursor);
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    free(cursor);

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement *statement, Table *table)
{
    if (statement->has_where_id)
    {
        Cursor *cursor = table_find(table, statement->target_id);
        void *node = get_page(table->pager, cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);

        if (cursor->cell_num >= num_cells || *leaf_node_key(node, cursor->cell_num) != statement->target_id)
        {
            free(cursor);
            return EXECUTE_NOT_FOUND;
        }

        Row row;
        deserialize_row(cursor_value(cursor), &row);
        print_row(&row);
        free(cursor);
        return EXECUTE_SUCCESS;
    }

    Cursor *cursor = table_start(table);

    Row row;
    while (!(cursor->end_of_table))
    {
        deserialize_row(cursor_value(cursor), &row);
        print_row(&row);
        cursor_advance(cursor);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_update(Statement *statement, Table *table)
{
    Row *row_to_update = &(statement->row_to_insert);
    Cursor *cursor = table_find(table, row_to_update->id);

    bool updated = leaf_node_update(cursor, row_to_update->id, row_to_update);
    free(cursor);

    return updated ? EXECUTE_SUCCESS : EXECUTE_NOT_FOUND;
}

ExecuteResult execute_delete(Statement *statement, Table *table)
{
    Cursor *cursor = table_find(table, statement->target_id);

    bool deleted = leaf_node_delete(cursor, statement->target_id);
    free(cursor);

    return deleted ? EXECUTE_SUCCESS : EXECUTE_NOT_FOUND;
}

ExecuteResult execute_statement(Statement *statement, Table *table)
{
    switch (statement->type)
    {
    case STATEMENT_INSERT:
        return execute_insert(statement, table);
    case STATEMENT_SELECT:
        return execute_select(statement, table);
    case STATEMENT_UPDATE:
        return execute_update(statement, table);
    case STATEMENT_DELETE:
        return execute_delete(statement, table);
    }
    return EXECUTE_SUCCESS;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    char *filename = argv[1];
    Table *table = db_open(filename);
    InputBuffer *input_buffer = new_input_buffer();

    while (true)
    {
        print_prompt();
        read_input(input_buffer);

        if (input_buffer->buffer[0] == '.')
        {
            switch (do_meta_command(input_buffer, table))
            {
            case META_COMMAND_SUCCESS:
                continue;
            case META_COMMAND_UNRECOGNIZED_COMMAND:
                printf("Unrecognized command '%s'\n", input_buffer->buffer);
                continue;
            }
        }

        Statement statement;
        switch (prepare_statement(input_buffer, &statement))
        {
        case PREPARE_SUCCESS:
            break;
        case PREPARE_NEGATIVE_ID:
            printf("ID must be positive.\n");
            continue;
        case PREPARE_STRING_TOO_LONG:
            printf("String is too long.\n");
            continue;
        case PREPARE_SYNTAX_ERROR:
            printf("Syntax error. Could not parse statement.\n");
            continue;
        case PREPARE_UNRECOGNIZED_STATEMENT:
            printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
            continue;
        }

        switch (execute_statement(&statement, table))
        {
        case EXECUTE_SUCCESS:
            printf("Executed.\n");
            break;
        case EXECUTE_DUPLICATE_KEY:
            printf("Error: Duplicate key.\n");
            break;
        case EXECUTE_NOT_FOUND:
            printf("Row not found.\n");
            break;
        case EXECUTE_TABLE_FULL:
            printf("Error: Table full.\n");
            break;
        }
    }
}