#ifndef OI_STATUS_H
#define OI_STATUS_H

/*
 * Status codes returned by fallible oi_* functions. 0 is always success;
 * callers that only care about success/failure can test `!= OI_OK`.
 */
typedef enum oi_status {
    OI_OK = 0,
    OI_ERR_INVAL,
    OI_ERR_NOMEM,
    OI_ERR_IO,      /* I/O error, see errno at the call site */
    OI_ERR_AGAIN,
    OI_ERR_PARSE,
    OI_ERR_CLOSED,
    OI_ERR_EXISTS,
    OI_ERR_NOTFOUND,
    OI_ERR_DENIED,  /* refused by caller-supplied policy, not a fault */
    OI_ERR_TIMEOUT
} oi_status;

/* Human-readable text for an oi_status, for diagnostic messages only --
 * not part of any wire format. */
static inline const char *oi_status_str(oi_status status) {
    switch (status) {
    case OI_OK:
        return "ok";
    case OI_ERR_INVAL:
        return "invalid argument";
    case OI_ERR_NOMEM:
        return "out of memory";
    case OI_ERR_IO:
        return "I/O error";
    case OI_ERR_AGAIN:
        return "would block";
    case OI_ERR_PARSE:
        return "parse error";
    case OI_ERR_CLOSED:
        return "closed";
    case OI_ERR_EXISTS:
        return "already exists";
    case OI_ERR_NOTFOUND:
        return "not found";
    case OI_ERR_DENIED:
        return "denied";
    case OI_ERR_TIMEOUT:
        return "timeout";
    }
    return "unknown error";
}

#endif
