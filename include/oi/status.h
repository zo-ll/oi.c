#ifndef OI_STATUS_H
#define OI_STATUS_H

typedef enum oi_status {
    OI_OK = 0,
    OI_ERR_INVAL,
    OI_ERR_NOMEM,
    OI_ERR_IO,
    OI_ERR_AGAIN,
    OI_ERR_PARSE,
    OI_ERR_CLOSED,
    OI_ERR_EXISTS,
    OI_ERR_NOTFOUND,
    OI_ERR_DENIED,
    OI_ERR_TIMEOUT
} oi_status;

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
