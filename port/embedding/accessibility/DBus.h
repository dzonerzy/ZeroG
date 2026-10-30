#pragma once

// libdbus, loaded at run time (ZeroG.md, "Cosa resta dinamico a runtime"): the
// AT-SPI bridge talks D-Bus only on systems that have it, and building needs no
// D-Bus headers. The types and constants are libdbus's public ABI (dbus/
// dbus-message.h, dbus-errors.h, dbus-protocol.h), stable since 1.0.

#include <cstdint>
#include <wtf/Function.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace ZeroG::DBus {

// libdbus takes UTF-8 as char.
inline const char* data(const UTF8CString& string)
{
    return byteCast<char>(string.data());
}

struct Connection;
struct Message;
struct Watch;

using Bool = uint32_t;

// DBusWatchFlags
enum WatchFlags : unsigned {
    WatchReadable = 1 << 0,
    WatchWritable = 1 << 1,
    WatchError = 1 << 2,
    WatchHangup = 1 << 3,
};

using AddWatchFunction = Bool (*)(Watch*, void* data);
using RemoveWatchFunction = void (*)(Watch*, void* data);
using WatchToggledFunction = void (*)(Watch*, void* data);
using FreeFunction = void (*)(void* data);

// DBusError
struct Error {
    const char* name { nullptr };
    const char* message { nullptr };
    unsigned dummy1 : 1 { 0 };
    unsigned dummy2 : 1 { 0 };
    unsigned dummy3 : 1 { 0 };
    unsigned dummy4 : 1 { 0 };
    unsigned dummy5 : 1 { 0 };
    void* padding1 { nullptr };
};

// DBusMessageIter
struct Iter {
    void* dummy1;
    void* dummy2;
    uint32_t dummy3;
    int dummy4, dummy5, dummy6, dummy7, dummy8, dummy9, dummy10, dummy11;
    int pad1;
    void* pad2;
    void* pad3;
};

enum Type : int {
    TypeInvalid = 0,
    TypeByte = 'y',
    TypeBoolean = 'b',
    TypeInt16 = 'n',
    TypeUInt16 = 'q',
    TypeInt32 = 'i',
    TypeUInt32 = 'u',
    TypeInt64 = 'x',
    TypeUInt64 = 't',
    TypeDouble = 'd',
    TypeString = 's',
    TypeObjectPath = 'o',
    TypeSignature = 'g',
    TypeArray = 'a',
    TypeVariant = 'v',
    TypeStruct = 'r',
    TypeDictEntry = 'e',
};

enum MessageType : int {
    MethodCall = 1,
    MethodReturn = 2,
    ErrorReply = 3,
    Signal = 4,
};

enum BusType : int { SessionBus = 0 };

struct Library {
    Bool (*threads_init_default)();
    void (*error_init)(Error*);
    void (*error_free)(Error*);
    Bool (*error_is_set)(const Error*);
    Connection* (*bus_get_private)(BusType, Error*);
    Connection* (*connection_open_private)(const char* address, Error*);
    Bool (*bus_register)(Connection*, Error*);
    const char* (*bus_get_unique_name)(Connection*);
    void (*bus_add_match)(Connection*, const char* rule, Error*);
    void (*connection_set_exit_on_disconnect)(Connection*, Bool);
    void (*connection_close)(Connection*);
    void (*connection_unref)(Connection*);
    Bool (*connection_send)(Connection*, Message*, uint32_t* serial);
    Message* (*connection_send_with_reply_and_block)(Connection*, Message*, int timeoutMilliseconds, Error*);
    Bool (*connection_read_write)(Connection*, int timeoutMilliseconds);
    Message* (*connection_pop_message)(Connection*);
    void (*connection_flush)(Connection*);
    Bool (*connection_get_is_connected)(Connection*);
    Bool (*connection_set_watch_functions)(Connection*, AddWatchFunction, RemoveWatchFunction, WatchToggledFunction, void* data, FreeFunction);
    int (*watch_get_unix_fd)(Watch*);
    unsigned (*watch_get_flags)(Watch*);
    Bool (*watch_get_enabled)(Watch*);
    Bool (*watch_handle)(Watch*, unsigned flags);
    Message* (*message_new_method_call)(const char* destination, const char* path, const char* interface, const char* method);
    Message* (*message_new_method_return)(Message*);
    Message* (*message_new_error)(Message*, const char* name, const char* message);
    Message* (*message_new_signal)(const char* path, const char* interface, const char* name);
    Message* (*message_ref)(Message*);
    void (*message_unref)(Message*);
    int (*message_get_type)(Message*);
    const char* (*message_get_path)(Message*);
    const char* (*message_get_interface)(Message*);
    const char* (*message_get_member)(Message*);
    const char* (*message_get_sender)(Message*);
    Bool (*message_iter_init)(Message*, Iter*);
    void (*message_iter_init_append)(Message*, Iter*);
    Bool (*message_iter_append_basic)(Iter*, int type, const void* value);
    Bool (*message_iter_open_container)(Iter*, int type, const char* containedSignature, Iter* sub);
    Bool (*message_iter_close_container)(Iter*, Iter* sub);
    int (*message_iter_get_arg_type)(Iter*);
    void (*message_iter_get_basic)(Iter*, void* value);
    Bool (*message_iter_next)(Iter*);
    void (*message_iter_recurse)(Iter*, Iter* sub);

    // Null when the system has no libdbus.
    static const Library* get();
};

// Appends a message's arguments.
class Writer {
public:
    explicit Writer(Message*);

    void string(const char*);
    void string(const String&);
    void objectPath(const char*);
    void boolean(bool);
    void int16(int16_t);
    void int32(int32_t);
    void uint32(uint32_t);
    void float64(double);
    // A container: array ('a', with its element signature), struct ('r'),
    // dict entry ('e') or variant ('v', with its value's signature).
    void container(int type, const char* signature, NOESCAPE const Function<void(Writer&)>&);

private:
    Writer() = default;
    Iter m_iter { };
};

// Reads a message's arguments in order.
class Reader {
public:
    explicit Reader(Message*);

    int type();
    bool next();
    bool boolean();
    int32_t int32();
    uint32_t uint32();
    double float64();
    String string(); // strings and object paths
    Reader recurse();

private:
    Reader() = default;
    Iter m_iter { };
    bool m_valid { false };
};

} // namespace ZeroG::DBus
