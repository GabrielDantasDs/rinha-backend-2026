#ifndef VALIDATION_H
#define VALIDATION_H

int validate_id(int id);
int validate_transaction(char *transaction);
int validate_customer(char *customer);
int validate_merchant(char *merchant);
int validate_terminal(char *terminal);
int validate_last_transaction(char *last_transaction);
int validate_request(char *request);

#endif