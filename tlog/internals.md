# Tlog Internals

The tlog program consists of three individual binaries, together they provide the terminal I/O recording and playback functionality:

  - `tlog-rec`: General purpose recording utility
  - `tlog-rec-session`: Terminal I/O recording program intended for use as the user's shell
  - `tlog-play`: Playback terminal I/O output recorded with **tlog**

Each program has it's own JSON configuration, the configuration is loaded in ascending order as outlined in the `tlog-rec` example below. Configuration options and parameters from each of the sources override the previous ones:

  - System default fallback configuration file, */usr/share/tlog/tlog-rec.conf*
  - System-wide configuration file, */etc/tlog/tlog-rec.conf*
  - File pointed to by the **TLOG_REC_CONF_FILE** environment variable
  - Contents of the **TLOG_REC_CONF_TEST** environment variable
  - Command-line-options

`tlog-rec` and `tlog-rec-session` share a majority of the same code, the main differences being that `tlog-rec-session` is intended to run as SUID/SGID to a separate dedicated user and group and therefore drops privileges on startup and only re-escalates priveleges as needed. `tlog-rec-session` reads the `shell` it will start for the recorded process from its configuration's `shell` option. `tlog-rec` reads the shell to use from the **SHELL** environment variable, or from NSS.

# Terminal I/O Recording, high-level

Tlog, and similar programs(script) rely on the OS Pseudoterminal(pty) device-pair in the kernel, to start a forked process under a pseudoterminal. Recording is commonly done by starting a user's shell under a pseudoterminal slave, this allows tlog to insert itself in between the terminal and user's shell to implement:

* transferring I/O bidirectionally
* Logging the intercepted I/O to a storage medium(file, journal, etc)

Tlog iteratively performs 2 sets of operations to accomplish this:

    - Read data available at the PTY master side, data(output) from the user's shell program
    - Log this data, if configured
    - Write this data to the terminal, this continues transferring I/O to its destination
    
    - Read data available(input) from the Terminal side
    - Log this data, if configured
    - Write this data to the PTY master

A visual representation of this data flow can be seen at:
https://en.wikipedia.org/wiki/Pseudoterminal#/media/File:Termios-script-diagram.svg

Excluding the logging part, a minimal implementation can be seen here: 
https://github.com/Scribery/tlog/commit/1d98119b9695d2f0410c16eae8c8d81e890c4a1b

The pty(7) and pts(4) man pages provide useful explanations about the pseudoterminal virtual device, and common usage.

# Recording

Tlog **recording** code implements the following abstract data types as modules:
- Tap: A "tap" is an I/O interception setup for an executed program.
- Source:
  - TTY source
- Sink
  - TTY sink
  - JSON (Log) sink
- Writer
  - JSON Writer
    - FD JSON Writer (File)
    - Journal JSON Writer
    - Syslog JSON Writer
    - Rate-limiting JSON Writer

#### Setup and initialization
Initializing the tap state creates the TTY source and TTY sink in `tlog_tap_setup`, this is where the *forkpty()* library call is made to create a new process operating in a pseudoterminal. After *forkpty()* returns, the PTY master file descriptor is assigned to *tap.in_fd* and *tap.out_fd*.
~~~
    /* Split master FD into input and output FDs */
    tap.in_fd = master_fd;
    tap.out_fd = dup(master_fd);
    if (tap.out_fd < 0) {
        grc = TLOG_GRC_ERRNO;
        TLOG_ERRS_RAISECS(grc, "Failed duplicating PTY master FD");
    }
~~~
The relevant file descriptors are now:
* **in_fd**, or tlog's STDIN_FILENO (terminal side)
* **out_fd**, or tlog's STDOUT_FILENO (terminal side)
* **tap.in_fd** (PTY side)
* **tap.out_fd** (PTY side)

TTY Source setup is done in `tlog_tty_source_init`. As indicated above tlog will read from 2 sources simultaneously,  file descriptors corresponding to the terminal(*in_fd* below) and PTY master-side(*tap.out_fd*, parameterized as *out_fd* below) are assigned to a *pollfd()* structure for I/O multiplexing.
~~~
    /* Read input from terminal */
    tty_source->fd_list[TLOG_TTY_SOURCE_FD_IDX_IN].fd = in_fd;
    tty_source->fd_list[TLOG_TTY_SOURCE_FD_IDX_IN].events = POLLIN;
    /* Read output from recorded process, out_fd == tap.out_fd here */
    tty_source->fd_list[TLOG_TTY_SOURCE_FD_IDX_OUT].fd = out_fd;
    tty_source->fd_list[TLOG_TTY_SOURCE_FD_IDX_OUT].events = POLLIN;
~~~
`tlog_tty_sink_create` creates a data sink using a similar approach as above to enable writing to *out_fd* and *tap.in_fd*, with no need for *poll()*
~~~
    /* Write input from the terminal as input to recorded program */
    tty_sink->in_fd = va_arg(ap, int);
    /* Write output from the recorded program to the terminal */
    tty_sink->out_fd = va_arg(ap, int);
    tty_sink->win_fd = va_arg(ap, int);
~~~
This sets up the necessary components to be able to transfer I/O bidirectionally, but the JSON(log) sink is needed to actually log the intercepted I/O. In `tlog_rec_create_log_sink` a writer is created based on the loaded configuration, the writer types are explained in the `tlog-rec` and `tlog-rec.conf` man pages. The writer type determines where the I/O will be logged and is self-explanatory, however note that the rate limiting writer cannot be defined as a 'writer' in the configuration, it is enabled on-demand.

#### Transferring I/O

The main recording transfer loop is `tlog_rec_transfer`, the high-level view can be outlined as:
- If file descriptor I/O is ready from the TTY source, read new data: `tlog_source_read`
- Log the received(source-read) data: `tlog_sink_write(log_sink, ...)`
- Deliver logged data to destination: `tlog_sink_write(tty_sink, ...)`
  
** **Note** this does not include handling latency limit, signal handling, etc.

##### Packet and position notes:

Data is read from the source into a packet, the packet is subsequently written to the Log and TTY sinks.
```
  include/tlog/pkt.h:
  
     * A packet stores any possible terminal data: window size changes or I/O.
     * A "void" packet is a packet not containing any data. Such packet can be
     * initialized to contain any data.

    /** Packet */
    struct tlog_pkt {
        struct timespec     timestamp;      /**< Timestamp */
        enum tlog_pkt_type  type;           /**< Packet type */
        union {
            struct tlog_pkt_data_window     window; /**< Window change data */
            struct tlog_pkt_data_io         io;     /**< I/O data */
        } data;                             /**< Type-specific data */
    };
    
    /** Packet type */
    enum tlog_pkt_type {
        TLOG_PKT_TYPE_VOID,     /**< Void (typeless) packet */
        TLOG_PKT_TYPE_WINDOW,   /**< Window size change */
        TLOG_PKT_TYPE_IO,       /**< I/O data */
        TLOG_PKT_TYPE_NUM       /**< Number of types (not a type itself) */
    };
```
After a successful TTY source read of *TLOG_PKT_TYPE_IO*, the *pkt->data.io.len* indicates the I/O data length read from the source
```
(gdb) p pkt->data.io.len
$8 = 4095
```
*log_pos* is used to maintain the logging packet position offset. This position offset is important because *tlog* needs to keep track of data which still needs to be logged, or has been logged already. *log_pos < packet length* represents data *tlog* needs to log. Once this data is written to the log sink, the *log_pos* value is increased appropriately.
```
    /* Log the received data, if any */
    if (tlog_pkt_pos_is_in(&log_pos, &pkt)) {
```
Similarly, *tty_pos* maintains the packet position offset for TTY sink data. Data needs to be written to the sink only when *tty_pos* is less than *log_pos*. This ensures *tlog* does not write any source-read data to the sink without logging it first. 
```
    /* Deliver logged data if any */
    if (tlog_pkt_pos_cmp(&tty_pos, &log_pos) < 0) {
```

##### Log sink notes:
- Writing data to the log sink buffer is done in chunks in `tlog_json_chunk_write`, the chunk size is based on the *payload* configuration value. Data is written to disk only when `tlog_json_writer_write` is called inside of `tlog_json_sink_flush`
```
    /* While the packet is not yet written completely */
    while (!tlog_json_chunk_write(&json_sink->chunk, pkt, ppos, end)) {
        grc = tlog_json_sink_flush(sink);
```
- Tlog message format is described in more detail here: https://github.com/Scribery/tlog/blob/master/doc/log_format.md

##### Window size changes
##### Rate Limiting

## Playback
