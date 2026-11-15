#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include "table.h"
#include "btree.h"
#include "wal.h"
#include "tokenizer.h"

typedef struct {
    char *buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_STRING_TOO_LONG,
    PREPARE_NEGATIVE_ID,
    PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_NOT_FOUND,
    EXECUTE_TABLE_FULL,
    EXECUTE_TXN_ALREADY_ACTIVE,
    EXECUTE_NO_ACTIVE_TXN
} ExecuteResult;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_UPDATE,
    STATEMENT_DELETE,
    STATEMENT_BEGIN,
    STATEMENT_COMMIT,
    STATEMENT_ROLLBACK
} StatementType;

typedef struct {
    StatementType type;
    Row row_to_insert;
    bool has_where_id;
    uint32_t target_id;
    bool update_username;
    bool update_email;
} Statement;

InputBuffer *new_input_buffer(void) {
    InputBuffer *input_buffer = malloc(sizeof(InputBuffer));
    if (input_buffer == NULL) {
        printf("Memory allocation failed for InputBuffer\n");
        exit(EXIT_FAILURE);
    }
    input_buffer->buffer = NULL;
    input_buffer->buffer_length = 0;
    input_buffer->input_length = 0;
    return input_buffer;
}

void close_input_buffer(InputBuffer *input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}

void print_prompt(bool in_txn) {
    if (in_txn) {
        printf("cqlite (txn) > ");
    } else {
        printf("cqlite > ");
    }
}

void read_input(InputBuffer *input_buffer) {
    ssize_t bytes_read = getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

    if (bytes_read <= 0) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }

    input_buffer->input_length = bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = 0;
}

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table) {
    if (strcmp(input_buffer->buffer, ".exit") == 0) {
        close_input_buffer(input_buffer);
        db_close(table);
        exit(EXIT_SUCCESS);
    }
    if (strcmp(input_buffer->buffer, ".checkpoint") == 0) {
        if (table->in_transaction) {
            printf("Cannot checkpoint while a transaction is active.\n");
            return META_COMMAND_SUCCESS;
        }
        wal_checkpoint(table->wal, table);
        printf("Checkpoint completed (dirty pages flushed, WAL truncated).\n");
        return META_COMMAND_SUCCESS;
    }
    if (strcmp(input_buffer->buffer, ".btree") == 0) {
        printf("Tree:\n");
        print_tree(table->pager, 0, 0);
        return META_COMMAND_SUCCESS;
    }
    if (strcmp(input_buffer->buffer, ".stats") == 0) {
        printf("Database Stats:\n");
        printf("  Page Size:        %d bytes\n", PAGE_SIZE);
        printf("  Row Size:         %lu bytes\n", (unsigned long)ROW_SIZE);
        printf("  Leaf Max Cells:   %lu\n", (unsigned long)LEAF_NODE_MAX_CELLS);
        printf("  Allocated Pages:  %u\n", table->pager->num_pages);
        printf("  Cached Pages:     %u / %d (Hits: %lu, Misses: %lu)\n",
               table->pager->cached_pages, CACHE_MAX_PAGES,
               table->pager->cache_hits, table->pager->cache_misses);
        printf("  Pending WAL Ops:  %lu\n",
               table->wal ? (unsigned long)table->wal->record_count : 0UL);
        printf("  Transaction:      %s\n", table->in_transaction ? "ACTIVE" : "AUTO-COMMIT");
        return META_COMMAND_SUCCESS;
    }
    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

static bool parse_uint32_token(const Token *tok, uint32_t *out, bool *is_negative) {
    *is_negative = false;
    if (tok->type != TOKEN_NUMBER) {
        return false;
    }
    if (tok->text[0] == '-') {
        *is_negative = true;
        return false;
    }
    char *endptr = NULL;
    errno = 0;
    unsigned long val = strtoul(tok->text, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || val > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)val;
    return true;
}

static bool expect_end_of_statement(TokenStream *ts) {
    while (expect_token(ts, TOKEN_SEMICOLON)) {
        /* allow optional trailing semicolons */
    }
    return peek_token(ts)->type == TOKEN_EOF;
}

static PrepareResult parse_where_id(TokenStream *ts, uint32_t *target_id) {
    if (!expect_keyword(ts, "id") || !expect_token(ts, TOKEN_EQUALS)) {
        return PREPARE_SYNTAX_ERROR;
    }
    const Token *id_tok = next_token(ts);
    bool is_neg = false;
    if (!parse_uint32_token(id_tok, target_id, &is_neg)) {
        return is_neg ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
    }
    return PREPARE_SUCCESS;
}

/*
 * Supports both:
 *   1) INSERT INTO users VALUES (1, 'Suvodeep Mishra', 'suvodeep@valko.studio');
 *   2) insert 1 'Suvodeep Mishra' suvodeep@valko.studio
 */
PrepareResult prepare_insert(TokenStream *ts, Statement *statement) {
    statement->type = STATEMENT_INSERT;
    next_token(ts); /* consume INSERT */

    bool sql_style = false;
    if (expect_keyword(ts, "into")) {
        sql_style = true;
        if (peek_token(ts)->type != TOKEN_IDENTIFIER) {
            return PREPARE_SYNTAX_ERROR;
        }
        next_token(ts); /* consume table name */
        if (!expect_keyword(ts, "values") || !expect_token(ts, TOKEN_LPAREN)) {
            return PREPARE_SYNTAX_ERROR;
        }
    }

    const Token *id_tok = next_token(ts);
    uint32_t id = 0;
    bool is_neg = false;
    if (!parse_uint32_token(id_tok, &id, &is_neg)) {
        return is_neg ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
    }

    if (sql_style && !expect_token(ts, TOKEN_COMMA)) {
        return PREPARE_SYNTAX_ERROR;
    }

    const Token *user_tok = next_token(ts);
    if (user_tok->type != TOKEN_STRING && user_tok->type != TOKEN_IDENTIFIER) {
        return PREPARE_SYNTAX_ERROR;
    }

    if (sql_style && !expect_token(ts, TOKEN_COMMA)) {
        return PREPARE_SYNTAX_ERROR;
    }

    const Token *email_tok = next_token(ts);
    if (email_tok->type != TOKEN_STRING && email_tok->type != TOKEN_IDENTIFIER) {
        return PREPARE_SYNTAX_ERROR;
    }

    if (sql_style && !expect_token(ts, TOKEN_RPAREN)) {
        return PREPARE_SYNTAX_ERROR;
    }

    if (!expect_end_of_statement(ts)) {
        return PREPARE_SYNTAX_ERROR;
    }

    if (strlen(user_tok->text) > COLUMN_USERNAME_SIZE || strlen(email_tok->text) > COLUMN_EMAIL_SIZE) {
        return PREPARE_STRING_TOO_LONG;
    }

    memset(&(statement->row_to_insert), 0, sizeof(Row));
    statement->row_to_insert.id = id;
    strcpy(statement->row_to_insert.username, user_tok->text);
    strcpy(statement->row_to_insert.email, email_tok->text);

    return PREPARE_SUCCESS;
}

/*
 * Supports:
 *   1) SELECT * FROM users;
 *   2) SELECT * FROM users WHERE id = 1;
 *   3) select / select where id = 1 / select 1
 */
PrepareResult prepare_select(TokenStream *ts, Statement *statement) {
    statement->type = STATEMENT_SELECT;
    statement->has_where_id = false;
    statement->target_id = 0;
    next_token(ts); /* consume SELECT */

    if (expect_token(ts, TOKEN_STAR)) {
        if (!expect_keyword(ts, "from") || peek_token(ts)->type != TOKEN_IDENTIFIER) {
            return PREPARE_SYNTAX_ERROR;
        }
        next_token(ts); /* consume table name */
    }

    if (expect_keyword(ts, "where")) {
        PrepareResult res = parse_where_id(ts, &statement->target_id);
        if (res != PREPARE_SUCCESS) {
            return res;
        }
        statement->has_where_id = true;
    } else if (peek_token(ts)->type == TOKEN_NUMBER) {
        bool is_neg = false;
        if (!parse_uint32_token(next_token(ts), &statement->target_id, &is_neg)) {
            return is_neg ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
        }
        statement->has_where_id = true;
    }

    if (!expect_end_of_statement(ts)) {
        return PREPARE_SYNTAX_ERROR;
    }
    return PREPARE_SUCCESS;
}

/*
 * Supports:
 *   1) UPDATE users SET username = 'New Name', email = 'a@b.com' WHERE id = 1;
 *   2) UPDATE users SET email = 'a@b.com' WHERE id = 1;
 *   3) update 1 'New Name' a@b.com
 */
PrepareResult prepare_update(TokenStream *ts, Statement *statement) {
    statement->type = STATEMENT_UPDATE;
    statement->update_username = false;
    statement->update_email = false;
    memset(&(statement->row_to_insert), 0, sizeof(Row));
    next_token(ts); /* consume UPDATE */

    /* Shorthand: update <id> <username> <email> */
    if (peek_token(ts)->type == TOKEN_NUMBER) {
        uint32_t id = 0;
        bool is_neg = false;
        if (!parse_uint32_token(next_token(ts), &id, &is_neg)) {
            return is_neg ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
        }
        const Token *user_tok = next_token(ts);
        const Token *email_tok = next_token(ts);
        if ((user_tok->type != TOKEN_STRING && user_tok->type != TOKEN_IDENTIFIER) ||
            (email_tok->type != TOKEN_STRING && email_tok->type != TOKEN_IDENTIFIER) ||
            !expect_end_of_statement(ts)) {
            return PREPARE_SYNTAX_ERROR;
        }
        if (strlen(user_tok->text) > COLUMN_USERNAME_SIZE || strlen(email_tok->text) > COLUMN_EMAIL_SIZE) {
            return PREPARE_STRING_TOO_LONG;
        }
        statement->row_to_insert.id = id;
        strcpy(statement->row_to_insert.username, user_tok->text);
        strcpy(statement->row_to_insert.email, email_tok->text);
        statement->update_username = true;
        statement->update_email = true;
        return PREPARE_SUCCESS;
    }

    /* SQL Syntax: UPDATE <table> SET col = val [, col = val] WHERE id = <id> */
    if (peek_token(ts)->type != TOKEN_IDENTIFIER) {
        return PREPARE_SYNTAX_ERROR;
    }
    next_token(ts); /* consume table name */

    if (!expect_keyword(ts, "set")) {
        return PREPARE_SYNTAX_ERROR;
    }

    do {
        const Token *col = next_token(ts);
        if (col->type != TOKEN_IDENTIFIER || !expect_token(ts, TOKEN_EQUALS)) {
            return PREPARE_SYNTAX_ERROR;
        }
        const Token *val = next_token(ts);
        if (val->type != TOKEN_STRING && val->type != TOKEN_IDENTIFIER) {
            return PREPARE_SYNTAX_ERROR;
        }
        if (strcasecmp(col->text, "username") == 0) {
            if (strlen(val->text) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
            strcpy(statement->row_to_insert.username, val->text);
            statement->update_username = true;
        } else if (strcasecmp(col->text, "email") == 0) {
            if (strlen(val->text) > COLUMN_EMAIL_SIZE) return PREPARE_STRING_TOO_LONG;
            strcpy(statement->row_to_insert.email, val->text);
            statement->update_email = true;
        } else {
            return PREPARE_SYNTAX_ERROR;
        }
    } while (expect_token(ts, TOKEN_COMMA));

    if (!expect_keyword(ts, "where")) {
        return PREPARE_SYNTAX_ERROR;
    }
    PrepareResult res = parse_where_id(ts, &statement->row_to_insert.id);
    if (res != PREPARE_SUCCESS) {
        return res;
    }
    if (!expect_end_of_statement(ts)) {
        return PREPARE_SYNTAX_ERROR;
    }

    return PREPARE_SUCCESS;
}

/*
 * Supports:
 *   1) DELETE FROM users WHERE id = 1;
 *   2) delete where id = 1 / delete 1
 */
PrepareResult prepare_delete(TokenStream *ts, Statement *statement) {
    statement->type = STATEMENT_DELETE;
    statement->has_where_id = true;
    next_token(ts); /* consume DELETE */

    if (expect_keyword(ts, "from")) {
        if (peek_token(ts)->type != TOKEN_IDENTIFIER) {
            return PREPARE_SYNTAX_ERROR;
        }
        next_token(ts); /* consume table name */
        if (!expect_keyword(ts, "where")) {
            return PREPARE_SYNTAX_ERROR;
        }
        PrepareResult res = parse_where_id(ts, &statement->target_id);
        if (res != PREPARE_SUCCESS) return res;
    } else if (expect_keyword(ts, "where")) {
        PrepareResult res = parse_where_id(ts, &statement->target_id);
        if (res != PREPARE_SUCCESS) return res;
    } else {
        bool is_neg = false;
        if (!parse_uint32_token(next_token(ts), &statement->target_id, &is_neg)) {
            return is_neg ? PREPARE_NEGATIVE_ID : PREPARE_SYNTAX_ERROR;
        }
    }

    if (!expect_end_of_statement(ts)) {
        return PREPARE_SYNTAX_ERROR;
    }
    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer *input_buffer, Statement *statement) {
    TokenStream ts;
    if (!tokenize_input(input_buffer->buffer, &ts)) {
        return PREPARE_SYNTAX_ERROR;
    }

    if (!tokenize_input(input_buffer->buffer, &ts)) {
        printf("Tokenizer error: %s\n", ts.error_msg);
        return PREPARE_SYNTAX_ERROR;
    }

    if (peek_token(&ts)->type == TOKEN_EOF) {
        return PREPARE_SYNTAX_ERROR;
    }

    if (match_keyword(&ts, "begin")) {
        next_token(&ts);
        return expect_end_of_statement(&ts) ? (statement->type = STATEMENT_BEGIN, PREPARE_SUCCESS) : PREPARE_SYNTAX_ERROR;
    }
    if (match_keyword(&ts, "commit")) {
        next_token(&ts);
        return expect_end_of_statement(&ts) ? (statement->type = STATEMENT_COMMIT, PREPARE_SUCCESS) : PREPARE_SYNTAX_ERROR;
    }
    if (match_keyword(&ts, "rollback")) {
        next_token(&ts);
        return expect_end_of_statement(&ts) ? (statement->type = STATEMENT_ROLLBACK, PREPARE_SUCCESS) : PREPARE_SYNTAX_ERROR;
    }
    if (match_keyword(&ts, "insert")) {
        return prepare_insert(&ts, statement);
    }
    if (match_keyword(&ts, "select")) {
        return prepare_select(&ts, statement);
    }
    if (match_keyword(&ts, "update")) {
        return prepare_update(&ts, statement);
    }
    if (match_keyword(&ts, "delete")) {
        return prepare_delete(&ts, statement);
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}

ExecuteResult execute_begin(Table *table) {
    if (table->in_transaction) {
        return EXECUTE_TXN_ALREADY_ACTIVE;
    }
    wal_checkpoint(table->wal, table);
    table->in_transaction = true;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_commit(Table *table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TXN;
    }
    if (table->wal != NULL) {
        fsync(table->wal->fd);
    }
    table->in_transaction = false;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback(Table *table) {
    if (!table->in_transaction) {
        return EXECUTE_NO_ACTIVE_TXN;
    }
    pager_discard_cache(table->pager);

    if (table->wal != NULL && ftruncate(table->wal->fd, 0) == 0) {
        lseek(table->wal->fd, 0, SEEK_SET);
        fsync(table->wal->fd);
        table->wal->record_count = 0;
    }

    if (table->pager->file_length == 0) {
        void *root_node = get_page(table->pager, 0);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);
        mark_page_dirty(table->pager, 0);
    }

    table->in_transaction = false;
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_insert(Statement *statement, Table *table) {
    Row *row_to_insert = &(statement->row_to_insert);
    uint32_t key_to_insert = row_to_insert->id;
    Cursor *cursor = table_find(table, key_to_insert);

    void *node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = (*leaf_node_num_cells(node));

    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
        if (key_at_index == key_to_insert) {
            free(cursor);
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    wal_append(table->wal, WAL_OP_INSERT, key_to_insert, row_to_insert, !table->in_transaction);

    leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    free(cursor);

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement *statement, Table *table) {
    if (statement->has_where_id) {
        Cursor *cursor = table_find(table, statement->target_id);
        void *node = get_page(table->pager, cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);

        if (cursor->cell_num >= num_cells || *leaf_node_key(node, cursor->cell_num) != statement->target_id) {
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
    while (!(cursor->end_of_table)) {
        deserialize_row(cursor_value(cursor), &row);
        print_row(&row);
        cursor_advance(cursor);
    }

    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_update(Statement *statement, Table *table) {
    Row *patch = &(statement->row_to_insert);
    Cursor *cursor = table_find(table, patch->id);
    void *node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells || *leaf_node_key(node, cursor->cell_num) != patch->id) {
        free(cursor);
        return EXECUTE_NOT_FOUND;
    }

    /* Merge existing fields with updated columns so single-column SET works */
    Row merged;
    deserialize_row(cursor_value(cursor), &merged);
    if (statement->update_username) {
        strcpy(merged.username, patch->username);
    }
    if (statement->update_email) {
        strcpy(merged.email, patch->email);
    }

    wal_append(table->wal, WAL_OP_UPDATE, merged.id, &merged, !table->in_transaction);
    leaf_node_update(cursor, merged.id, &merged);
    free(cursor);

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_delete(Statement *statement, Table *table) {
    Cursor *cursor = table_find(table, statement->target_id);
    void *node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells || *leaf_node_key(node, cursor->cell_num) != statement->target_id) {
        free(cursor);
        return EXECUTE_NOT_FOUND;
    }

    wal_append(table->wal, WAL_OP_DELETE, statement->target_id, NULL, !table->in_transaction);
    leaf_node_delete(cursor, statement->target_id);
    free(cursor);

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement *statement, Table *table) {
    switch (statement->type) {
    case STATEMENT_BEGIN:
        return execute_begin(table);
    case STATEMENT_COMMIT:
        return execute_commit(table);
    case STATEMENT_ROLLBACK:
        return execute_rollback(table);
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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    char *filename = argv[1];
    Table *table = db_open(filename);
    InputBuffer *input_buffer = new_input_buffer();

    while (true) {
        print_prompt(table->in_transaction);
        read_input(input_buffer);

        if (input_buffer->buffer[0] == '.') {
            switch (do_meta_command(input_buffer, table)) {
            case META_COMMAND_SUCCESS:
                continue;
            case META_COMMAND_UNRECOGNIZED_COMMAND:
                printf("Unrecognized command '%s'\n", input_buffer->buffer);
                continue;
            }
        }

        Statement statement;
        switch (prepare_statement(input_buffer, &statement)) {
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

        switch (execute_statement(&statement, table)) {
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
        case EXECUTE_TXN_ALREADY_ACTIVE:
            printf("Error: Transaction already active.\n");
            break;
        case EXECUTE_NO_ACTIVE_TXN:
            printf("Error: No active transaction.\n");
            break;
        }
    }
}
