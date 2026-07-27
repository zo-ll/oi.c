#ifndef OI_STATUS_H
#define OI_STATUS_H

/*
 * Status codes returned by fallible oi_* functions. 0 is always success;
 * callers that only care about success/failure can test `!= OI_OK`.
 */
typedef enum oi_status {
    OI_OK = 0,
    OI_ERR_INVAL,   /* invalid argument */
    OI_ERR_NOMEM,   /* allocation failed */
    OI_ERR_IO,      /* I/O error, see errno at the call site */
    OI_ERR_AGAIN,   /* would block; retry later */
    OI_ERR_PARSE,   /* malformed input */
    OI_ERR_CLOSED,  /* peer/resource closed */
    OI_ERR_EXISTS,  /* duplicate id/key */
    OI_ERR_NOTFOUND /* id/key not found */
} oi_status;

#endif /* OI_STATUS_H */
