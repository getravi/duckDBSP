#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include <iostream>

using namespace duckdb;
using namespace std;

void print_node_type(QueryNode *node) {
  if (node->type == QueryNodeType::SELECT_NODE) {
    cout << "Type: SELECT_NODE" << endl;
  } else if (node->type == QueryNodeType::SET_OPERATION_NODE) {
    auto set_op = (SetOperationNode *)node;
    cout << "Type: SET_OPERATION_NODE" << endl;
    cout << "Operation: ";
    switch (set_op->setop_type) {
    case SetOperationType::UNION:
      cout << "UNION";
      break;
    case SetOperationType::EXCEPT:
      cout << "EXCEPT";
      break;
    case SetOperationType::INTERSECT:
      cout << "INTERSECT";
      break;
    default:
      cout << "UNKNOWN";
      break;
    }
    cout << endl;

    cout << "Left: ";
    print_node_type(set_op->left.get());
    cout << "Right: ";
    print_node_type(set_op->right.get());
  } else {
    cout << "Type: OTHER (" << (int)node->type << ")" << endl;
  }
}

int main() {
  Parser parser;

  // Test UNION
  cout << "--- UNION ---" << endl;
  parser.ParseQuery("SELECT 1 UNION SELECT 2");
  if (!parser.statements.empty() &&
      parser.statements[0]->type == StatementType::SELECT_STATEMENT) {
    auto &select = (SelectStatement &)*parser.statements[0];
    print_node_type(select.node.get());
  }

  // Test INTERSECT
  cout << "\n--- INTERSECT ---" << endl;
  parser.ParseQuery("SELECT 1 INTERSECT SELECT 2");
  if (!parser.statements.empty() &&
      parser.statements[0]->type == StatementType::SELECT_STATEMENT) {
    auto &select = (SelectStatement &)*parser.statements[0];
    print_node_type(select.node.get());
  }

  // Test EXCEPT
  cout << "\n--- EXCEPT ---" << endl;
  parser.ParseQuery("SELECT 1 EXCEPT SELECT 2");
  if (!parser.statements.empty() &&
      parser.statements[0]->type == StatementType::SELECT_STATEMENT) {
    auto &select = (SelectStatement &)*parser.statements[0];
    print_node_type(select.node.get());
  }

  // Test Nested
  cout << "\n--- NESTED ---" << endl;
  parser.ParseQuery("SELECT 1 UNION SELECT 2 UNION SELECT 3");
  if (!parser.statements.empty() &&
      parser.statements[0]->type == StatementType::SELECT_STATEMENT) {
    auto &select = (SelectStatement &)*parser.statements[0];
    print_node_type(select.node.get());
  }

  return 0;
}
