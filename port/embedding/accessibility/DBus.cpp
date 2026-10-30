#include "config.h"
#include "accessibility/DBus.h"

#if !OS(WINDOWS)

#include <dlfcn.h>
#include <wtf/NeverDestroyed.h>

namespace ZeroG::DBus {

const Library* Library::get()
{
    static NeverDestroyed<std::optional<Library>> library = []() -> std::optional<Library> {
        void* handle = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            return std::nullopt;
        Library result;
        bool complete = true;
        auto load = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(handle, name));
            complete &= !!function;
        };
        load(result.threads_init_default, "dbus_threads_init_default");
        load(result.error_init, "dbus_error_init");
        load(result.error_free, "dbus_error_free");
        load(result.error_is_set, "dbus_error_is_set");
        load(result.bus_get_private, "dbus_bus_get_private");
        load(result.connection_open_private, "dbus_connection_open_private");
        load(result.bus_register, "dbus_bus_register");
        load(result.bus_get_unique_name, "dbus_bus_get_unique_name");
        load(result.bus_add_match, "dbus_bus_add_match");
        load(result.connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
        load(result.connection_close, "dbus_connection_close");
        load(result.connection_unref, "dbus_connection_unref");
        load(result.connection_send, "dbus_connection_send");
        load(result.connection_send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
        load(result.connection_read_write, "dbus_connection_read_write");
        load(result.connection_pop_message, "dbus_connection_pop_message");
        load(result.connection_flush, "dbus_connection_flush");
        load(result.message_new_method_call, "dbus_message_new_method_call");
        load(result.message_new_method_return, "dbus_message_new_method_return");
        load(result.message_new_error, "dbus_message_new_error");
        load(result.message_new_signal, "dbus_message_new_signal");
        load(result.message_ref, "dbus_message_ref");
        load(result.message_unref, "dbus_message_unref");
        load(result.message_get_type, "dbus_message_get_type");
        load(result.message_get_path, "dbus_message_get_path");
        load(result.message_get_interface, "dbus_message_get_interface");
        load(result.message_get_member, "dbus_message_get_member");
        load(result.message_get_sender, "dbus_message_get_sender");
        load(result.message_iter_init, "dbus_message_iter_init");
        load(result.message_iter_init_append, "dbus_message_iter_init_append");
        load(result.message_iter_append_basic, "dbus_message_iter_append_basic");
        load(result.message_iter_open_container, "dbus_message_iter_open_container");
        load(result.message_iter_close_container, "dbus_message_iter_close_container");
        load(result.message_iter_get_arg_type, "dbus_message_iter_get_arg_type");
        load(result.message_iter_get_basic, "dbus_message_iter_get_basic");
        load(result.message_iter_next, "dbus_message_iter_next");
        load(result.message_iter_recurse, "dbus_message_iter_recurse");
        if (!complete)
            return std::nullopt;
        // Connections are used from the bridge's thread and the engine thread.
        result.threads_init_default();
        return result;
    }();
    return library->has_value() ? &library->value() : nullptr;
}

static const Library& library()
{
    return *Library::get();
}

Writer::Writer(Message* message)
{
    library().message_iter_init_append(message, &m_iter);
}

void Writer::string(const char* value)
{
    library().message_iter_append_basic(&m_iter, TypeString, &value);
}

void Writer::string(const String& value)
{
    // D-Bus strings are UTF-8 without NUL characters.
    auto utf8 = value.utf8();
    string(data(utf8));
}

void Writer::objectPath(const char* value)
{
    library().message_iter_append_basic(&m_iter, TypeObjectPath, &value);
}

void Writer::boolean(bool value)
{
    Bool number = value;
    library().message_iter_append_basic(&m_iter, TypeBoolean, &number);
}

void Writer::int16(int16_t value)
{
    library().message_iter_append_basic(&m_iter, TypeInt16, &value);
}

void Writer::int32(int32_t value)
{
    library().message_iter_append_basic(&m_iter, TypeInt32, &value);
}

void Writer::uint32(uint32_t value)
{
    library().message_iter_append_basic(&m_iter, TypeUInt32, &value);
}

void Writer::float64(double value)
{
    library().message_iter_append_basic(&m_iter, TypeDouble, &value);
}

void Writer::container(int type, const char* signature, NOESCAPE const Function<void(Writer&)>& contents)
{
    Writer sub;
    library().message_iter_open_container(&m_iter, type, signature, &sub.m_iter);
    contents(sub);
    library().message_iter_close_container(&m_iter, &sub.m_iter);
}

Reader::Reader(Message* message)
{
    m_valid = library().message_iter_init(message, &m_iter);
}

int Reader::type()
{
    return m_valid ? library().message_iter_get_arg_type(&m_iter) : TypeInvalid;
}

bool Reader::next()
{
    return m_valid && library().message_iter_next(&m_iter);
}

bool Reader::boolean()
{
    Bool value = 0;
    if (type() == TypeBoolean)
        library().message_iter_get_basic(&m_iter, &value);
    return value;
}

int32_t Reader::int32()
{
    int32_t value = 0;
    if (type() == TypeInt32)
        library().message_iter_get_basic(&m_iter, &value);
    return value;
}

uint32_t Reader::uint32()
{
    uint32_t value = 0;
    if (type() == TypeUInt32)
        library().message_iter_get_basic(&m_iter, &value);
    return value;
}

double Reader::float64()
{
    double value = 0;
    if (type() == TypeDouble)
        library().message_iter_get_basic(&m_iter, &value);
    return value;
}

String Reader::string()
{
    const char* value = nullptr;
    if (type() == TypeString || type() == TypeObjectPath || type() == TypeSignature)
        library().message_iter_get_basic(&m_iter, &value);
    return value ? String::fromUTF8(value) : String();
}

Reader Reader::recurse()
{
    Reader sub;
    int containerType = type();
    if (containerType == TypeArray || containerType == TypeStruct || containerType == TypeVariant || containerType == TypeDictEntry) {
        library().message_iter_recurse(&m_iter, &sub.m_iter);
        sub.m_valid = true;
    }
    return sub;
}

} // namespace ZeroG::DBus

#endif // !OS(WINDOWS)
