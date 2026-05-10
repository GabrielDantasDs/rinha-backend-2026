#include "stddef.h"
#include <stdio.h>
#include <string.h>

int validate_id(int id)
{
    return id > 0;
}

int validate_transaction(char *transaction)
{
    // Implement your transaction validation logic here
    // For example, check if the transaction string is not empty

    char *amount = strstr(transaction, "amount");

    if (amount == NULL)
    {
        return 0; // Invalid transaction
    }

    char *installments = strstr(transaction, "installments");

    if (installments == NULL)
    {
        return 0; // Invalid transaction
    }

    char *requested_at = strstr(transaction, "requested_at");

    if (requested_at == NULL)
    {
        return 0; // Invalid transaction
    }

    if (transaction == NULL)
    {
        return 0; // Invalid transaction
    }

    if (strlen(transaction) <= 0)
    {
        return 0; // Invalid transacttion
    }

    return 1; // Valid transaction
}

int validate_customer (char *customer)
{
    // Implement your customer validation logic here
    // For example, check if the customer string is not empty

    char *avg_amount = strstr(customer, "avg_amount");

    if (avg_amount == NULL)
    {
        return 0; // Invalid customer
    }

    char *tx_count_24h = strstr(customer, "tx_count_24h");

    if (tx_count_24h == NULL)
    {
        return 0; // Invalid customer
    }

    char *known_merchants = strstr(customer, "known_merchants");

    if (known_merchants == NULL)
    {
        return 0;
    }

    if (customer == NULL)
    {
        return 0;
    }

    if (strlen(customer) <= 0)
    {
        return 0; 
    }

    return 1;
}

int validate_merchant(char *merchant)
{
    // Implement your merchant validation logic here
    // For example, check if the merchant string is not empty

    char *avg_amount = strstr(merchant, "avg_amount");

    if (avg_amount == NULL)
    {
        return 0; // Invalid merchant
    }

    char *id = strstr(merchant, "id");

    if (id == NULL)
    {
        return 0; // Invalid merchant
    }

    char *mcc = strstr(merchant, "mcc");

    if (mcc == NULL)
    {
        return 0; // Invalid merchant
    }

    if (merchant == NULL)
    {
        return 0;
    }

    if (strlen(merchant) <= 0)
    {
        return 0; 
    }

    return 1;
}

int validate_terminal (char *terminal) {

    if (terminal == NULL)
    {
        return 0; // Invalid terminal
    }

    if (strlen(terminal) <= 0)
    {
        return 0; // Invalid terminal
    }

    char *is_online = strstr(terminal, "is_online");

    if (is_online == NULL)
    {
        return 0; // Invalid terminal
    }

    char *card_present = strstr(terminal, "card_present");

    if (card_present == NULL)
    {
        return 0; // Invalid terminal
    }

    char *km_from_home = strstr(terminal, "km_from_home");

    if (km_from_home == NULL)
    {
        return 0; // Invalid terminal
    }

    return 1; // Valid terminal
}

int validate_last_transaction (char *last_transaction) {

    if (last_transaction == NULL)
    {
        return 1;
    }

    if (strlen(last_transaction) <= 0)
    {
        return 0; // Invalid last transaction
    }

    char *timestamp = strstr(last_transaction, "timestamp");

    if (timestamp == NULL)
    {
        return 0; // Invalid last transaction
    }

    char *km_from_current = strstr(last_transaction, "km_from_current");

    if (km_from_current == NULL)
    {
        return 0; // Invalid last transaction
    }

    return 1; // Valid last transaction
}  

int validate_request (char *request) {

    if (request == NULL)
    {
        return 0; // Invalid request
    }

    if (strlen(request) <= 0)
    {
        return 0; // Invalid request
    }

    /* Match each key as a quoted JSON key ("\"name\"") so substring
     * collisions like "transaction" inside "last_transaction" or
     * "merchant" inside "known_merchants" don't produce false positives. */
    if (strstr(request, "\"transaction\"") == NULL)
    {
        return 0;
    }

    if (strstr(request, "\"customer\"") == NULL)
    {
        return 0;
    }

    if (strstr(request, "\"merchant\"") == NULL)
    {
        return 0;
    }

    if (strstr(request, "\"terminal\"") == NULL)
    {
        return 0;
    }

    if (strstr(request, "\"last_transaction\"") == NULL)
    {
        return 0;
    }

    return 1; // Valid request
}