/* Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#include "sql_show_status.h"

#include "m_string.h"                  // C_STRING_WITH_LEN
#include "mysql/mysql_lex_string.h"    // LEX_STRING
#include "thr_lock.h"                  // TL_READ
#include "item_cmpfunc.h"              // Item_func_like
#include "parse_tree_items.h"          // PTI_simple_ident_ident
#include "parse_tree_nodes.h"          // PT_select_item_list
#include "sql_class.h"                 // THD
#include "sql_cmd.h"                   // enum_sql_command
#include "sql_lex.h"                   // Query_options


/**
  Build a replacement query for SHOW STATUS.
  When the parser accepts the following syntax:
    SHOW GLOBAL STATUS
  the parsed tree built for this query is in fact:
    SELECT VARIABLE_NAME as Variable_name,
           VARIABLE_VALUE as Value
           FROM performance_schema.global_status

  Likewise, the query:
    SHOW GLOBAL STATUS LIKE "<value>"
  is built as:
    SELECT VARIABLE_NAME as Variable_name,
           VARIABLE_VALUE as Value
           FROM performance_schema.global_status
           WHERE Variable_name LIKE "<value>"
*/
static SELECT_LEX*
build_query(const POS &pos,
            THD *thd,
            enum_sql_command command,
            const LEX_STRING& table_name,
            const String *wild,
            Item *where_cond)
{
  /*
    MAINTAINER:
    This code builds a parsed tree for a query.
    Write the query to build in SQL first,
    then see turn_parser_debug_on() in sql_yacc.yy
    to understand which grammar actions are needed to
    build a parsed tree for this SQL query.
  */
  static const LEX_STRING col_name= { C_STRING_WITH_LEN("VARIABLE_NAME")};
  static const LEX_STRING as_name= { C_STRING_WITH_LEN("Variable_name")};
  static const LEX_STRING col_value= { C_STRING_WITH_LEN("VARIABLE_VALUE")};
  static const LEX_STRING as_value= { C_STRING_WITH_LEN("Value")};
  static const LEX_STRING pfs= { C_STRING_WITH_LEN("performance_schema")};

  static const Query_options options=
  {
    0, /* query_spec_options */
    SELECT_LEX::SQL_CACHE_UNSPECIFIED /* sql_cache */
  };

  /* ... VARIABLE_NAME ... */
  PTI_simple_ident_ident *ident_name;
  ident_name= new (thd->mem_root) PTI_simple_ident_ident(pos, col_name);
  if (ident_name == NULL)
    return NULL;

  /* ... VARIABLE_NAME as Variable_name ... */
  PTI_expr_with_alias *expr_name;
  expr_name= new (thd->mem_root) PTI_expr_with_alias(pos, ident_name, pos.cpp, as_name);
  if (expr_name == NULL)
    return NULL;

  /* ... VARIABLE_VALUE ... */
  PTI_simple_ident_ident *ident_value;
  ident_value= new (thd->mem_root) PTI_simple_ident_ident(pos, col_value);
  if (ident_value == NULL)
    return NULL;

  /* ... VARIABLE_VALUE as Value ... */
  PTI_expr_with_alias *expr_value;
  expr_value= new (thd->mem_root) PTI_expr_with_alias(pos, ident_value, pos.cpp, as_value);
  if (expr_value == NULL)
    return NULL;

  /* ... VARIABLE_NAME as Variable_name, VARIABLE_VALUE as Value ... */
  PT_select_item_list *item_list;
  item_list= new (thd->mem_root) PT_select_item_list();
  if (item_list == NULL)
    return NULL;
  item_list->push_back(expr_name);
  item_list->push_back(expr_value);

  /* SELECT VARIABLE_NAME as Variable_name, VARIABLE_VALUE as Value ... */
  PT_select_options_and_item_list *options_and_item_list;
  options_and_item_list= new (thd->mem_root) PT_select_options_and_item_list(options, item_list);
  if (options_and_item_list == NULL)
    return NULL;

  /*
    make_table_list() might alter the database and table name strings. Create
    copies and leave the original values unaltered.
  */

  /* ... performance_schema ... */
  LEX_CSTRING tmp_db_name;
  if (!thd->make_lex_string(&tmp_db_name, pfs.str, pfs.length, false))
    return NULL;

  /* ... <table_name> ... */
  LEX_CSTRING tmp_table_name;
  if (!thd->make_lex_string(&tmp_table_name, table_name.str, table_name.length, false))
    return NULL;

  /* ... performance_schema.<table_name> ... */
  Table_ident *table_ident;
  table_ident= new (thd->mem_root) Table_ident(tmp_db_name, tmp_table_name);
  if (table_ident == NULL)
    return NULL;

  /* ... FROM performance_schema.<table_name> ... */
  PT_table_factor_table_ident *table_factor;
  table_factor= new (thd->mem_root) PT_table_factor_table_ident(table_ident, NULL, NULL, NULL);
  if (table_factor == NULL)
    return NULL;

  PT_table_reference_list *table_reference_list;
  table_reference_list= new (thd->mem_root) PT_table_reference_list(pos, table_factor);
  if (table_reference_list == NULL)
    return NULL;

  Item *where_clause= NULL;

  if (wild != NULL)
  {
    /* ... Variable_name ... */
    PTI_simple_ident_ident *ident_name_where;
    ident_name_where= new (thd->mem_root) PTI_simple_ident_ident(pos, as_name);
    if (ident_name_where == NULL)
      return NULL;

    /* ... <value> ... */
    LEX_STRING *lex_string;
    lex_string= static_cast<LEX_STRING*> (thd->alloc(sizeof(LEX_STRING)));
    if (lex_string == NULL)
      return NULL;
    lex_string->length= wild->length();
    lex_string->str= thd->strmake(wild->ptr(), wild->length());
    if (lex_string->str == NULL)
      return NULL;

    PTI_text_literal_text_string *wild_string;
    wild_string= new (thd->mem_root) PTI_text_literal_text_string(pos, false, *lex_string); // TODO WL#6629 check is_7bit
    if (wild_string == NULL)
      return NULL;

    /* ... Variable_name LIKE <value> ... */
    Item_func_like *func_like;
    func_like= new (thd->mem_root) Item_func_like(pos, ident_name_where, wild_string, NULL);
    if (func_like == NULL)
      return NULL;

    /* ... WHERE Variable_name LIKE <value> ... */
    where_clause= new (thd->mem_root) PTI_context<CTX_WHERE>(pos, func_like);
    if (where_clause == NULL)
      return NULL;
  }
  else
  {
    where_clause= where_cond;
  }

  /* SELECT ... [ WHERE Variable_name LIKE <value> ] */
  /* SELECT ... [ WHERE <cond> ] */
  PT_select_part2 *select_part2;
  select_part2= new (thd->mem_root) PT_select_part2(options_and_item_list,
                                                    table_reference_list,
                                                    where_clause);
  if (select_part2 == NULL)
    return NULL;

  PT_query_primary *query_primary=
    new (thd->mem_root) PT_query_specification(select_part2);
  if (query_primary == NULL)
    return NULL;

  PT_query_expression_body_primary *query_expression_body=
    new (thd->mem_root) PT_query_expression_body_primary(query_primary);
  if (query_primary == NULL)
    return NULL;

   PT_query_expression *query_expression=
     new (thd->mem_root) PT_query_expression(query_expression_body);
   if (query_expression == NULL)
     return NULL;

  PT_select_stmt *select;
  select= new (thd->mem_root) PT_select_stmt(query_expression);
  if (select == NULL)
    return NULL;

  LEX *lex= thd->lex;
  SELECT_LEX *current_select= lex->current_select();
  Parse_context pc(thd, current_select);
  if (thd->is_error())
    return NULL;

  if (select->contextualize(&pc))
    return NULL;

  /* contextualize sets to COM_SELECT */
  lex->sql_command= command;

  return current_select;
}

SELECT_LEX*
build_show_session_status(const POS &pos, THD *thd, const String *wild, Item *where_cond)
{
  static const LEX_STRING table_name= { C_STRING_WITH_LEN("session_status")};

  return build_query(pos, thd, SQLCOM_SHOW_STATUS, table_name, wild, where_cond);
}

SELECT_LEX*
build_show_global_status(const POS &pos, THD *thd, const String *wild, Item *where_cond)
{
  static const LEX_STRING table_name= { C_STRING_WITH_LEN("global_status")};

  return build_query(pos, thd, SQLCOM_SHOW_STATUS, table_name, wild, where_cond);
}

SELECT_LEX*
build_show_session_variables(const POS &pos, THD *thd, const String *wild, Item *where_cond)
{
  static const LEX_STRING table_name= { C_STRING_WITH_LEN("session_variables")};

  return build_query(pos, thd, SQLCOM_SHOW_VARIABLES, table_name, wild, where_cond);
}

SELECT_LEX*
build_show_global_variables(const POS &pos, THD *thd, const String *wild, Item *where_cond)
{
  static const LEX_STRING table_name= { C_STRING_WITH_LEN("global_variables")};

  return build_query(pos, thd, SQLCOM_SHOW_VARIABLES, table_name, wild, where_cond);
}

