# KCM Notes ( WIP )

-- Secdb secrets backend is the default backend, other backends(mem.c and
secrets.c) are undocumented, not for production use.

Links:
* [SSSD KCM Design](https://docs.pagure.org/sssd.sssd/design_pages/kcm.html)
* [MIT Client Library](https://github.com/krb5/krb5.git)
* [MIT Client Library KCM Client](https://github.com/krb5/krb5/blob/master/src/lib/krb5/ccache/cc_kcm.c)
* [MIT Client Library KCM Client Design(Wiki)](https://k5wiki.kerberos.org/wiki/Projects/KCM_client)
* [Heimdal KCM (Server) CCache implementation](https://github.com/heimdal/heimdal/tree/master/kcm)

Protocol:
```
/*
 * All requests begin with:
 *   major version (1 bytes)
 *   minor version (1 bytes)
 *   opcode (16-bit big-endian)
 *
 * All replies begin with a 32-bit big-endian reply code.
 *
 * Parameters are appended to the request or reply with no delimiters.  Flags
 * and time offsets are stored as 32-bit big-endian integers.  Names are
 * marshalled as zero-terminated strings.  Principals and credentials are
 * marshalled in the v4 FILE ccache format.  UUIDs are 16 bytes.  UUID lists
 * are not delimited, so nothing can come after them.
 */

/* Opcodes without comments are currently unused in the MIT client
 * implementation. */
typedef enum kcm_opcode {
    KCM_OP_NOOP,
    KCM_OP_GET_NAME,
    KCM_OP_RESOLVE,
    KCM_OP_GEN_NEW,             /* 0x3                 () -> (name)      */
    KCM_OP_INITIALIZE,          /* 0x4      (name, princ) -> ()          */
    KCM_OP_DESTROY,             /* 0x4             (name) -> ()          */
    KCM_OP_STORE,               /* 0x6       (name, cred) -> ()          */
    KCM_OP_RETRIEVE,
    KCM_OP_GET_PRINCIPAL,       /* 0x8             (name) -> (princ)     */
    KCM_OP_GET_CRED_UUID_LIST,  /* 0x9             (name) -> (uuid, ...) */
    KCM_OP_GET_CRED_BY_UUID,    /* 0xa       (name, uuid) -> (cred)      */
    KCM_OP_REMOVE_CRED,         /* (name, flags, credtag) -> ()          */
    KCM_OP_SET_FLAGS,
    KCM_OP_CHOWN,
    KCM_OP_CHMOD,
    KCM_OP_GET_INITIAL_TICKET,
    KCM_OP_GET_TICKET,
    KCM_OP_MOVE_CACHE,
    KCM_OP_GET_CACHE_UUID_LIST, /* 0x12                () -> (uuid, ...) */
    KCM_OP_GET_CACHE_BY_UUID,   /* 0x13            (uuid) -> (name)      */
    KCM_OP_GET_DEFAULT_CACHE,   /* 0x14                () -> (name)      */
    KCM_OP_SET_DEFAULT_CACHE,   /* 0x15            (name) -> ()          */
    KCM_OP_GET_KDC_OFFSET,      /* 0x16            (name) -> (offset)    */
    KCM_OP_SET_KDC_OFFSET,      /* 0x17    (name, offset) -> ()          */
    KCM_OP_ADD_NTLM_CRED,
    KCM_OP_HAVE_NTLM_CRED,
    KCM_OP_DEL_NTLM_CRED,
    KCM_OP_DO_NTLM_AUTH,
    KCM_OP_GET_NTLM_USER_LIST,

    KCM_OP_SENTINEL,            /* SSSD addition, not in the MIT header */
} kcm_opcode;
```


Debugging: 
* libkrb5: **KRB5_TRACE=/dev/stdout kinit -V...**
* SSSD:
```
[kcm]
debug_level = 10

# systemctl restart sssd-kcm
```
**This will quickly generate large log files. Turn off when not needed.**

* KCM Protocol request/response: Dump of the KCM socket data, intermixed with sssd KCM debug logging:
~~~
sudo strace -p $(pidof sssd_kcm) -Ttxvs 1024 -e read=$fds -e read,readv,write
~~~


-- Tracking a basic 'destroy' operation

1) Run 'kdestroy' on the command-line, or an application calls `krb5_cc_destroy`

2) **KCM_OP_DESTROY** is sent by *libkrb5* with a marshalled name parameter

    KCM_OP_DESTROY,             /* 0x4             (name) -> ()          */

**Now on the sssd-kcm side**
3) Read from the unix domain socket iov 

4) Parse the request input in kcm_input_parse()
```
 * The request is received as two IO vectors:
 *
 * first iovec:
 *  length                      32-bit big-endian integer
 *
 * second iovec:
 *  major protocol number       8-bit big-endian integer
 *  minor protocol number       8-bit big-endian integer
 *  opcode                      16-bit big-endian integer
 *  message payload             buffer
```

5) Validate the version numbers and retrieve the op code and payload, the KCM operation(function below in kcmsrv_ops.c) receives only the payload

5) Call the SSSD KCM function for **KCM_OP_DESTROY**

2) In `kcm_op_destroy_send()`, sssd unmarshall's the input name

3) In kcmsrv_ops.c, call function to find ccache by name(opaque interface) - this calls the underlying ccache storage function:
    `kcm_ccdb_uuid_by_name_send()`
     -> `db->ops->uuid_by_name_send() `
       -> `ccdb_secdb_uuid_by_name_send()`

4) With returned UUID, perform similar logic as above 

    `kcm_ccdb_delete_cc_send()`
     -> `state->db->ops->delete_send()`
       -> `ccdb_secdb_delete_send()`

5) Check if the deleted ccache was the default, and reset the default if so

6) Return EOK response status code to *libkrb5*, **KCM_OP_DESTROY** has no response parameters

