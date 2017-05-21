//
//  Copyright (C) 2017 Danny Havenith
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#include "client.hpp"
#include <stdlib.h>
#include "command_codes.hpp"

namespace
{
    constexpr uint8_t SLIP_END     = 0xC0;    /**< End of packet */
    constexpr uint8_t SLIP_ESC     = 0xDB;    /**< Escape */
    constexpr uint8_t SLIP_ESC_END = 0xDC;    /**< Escaped END */
    constexpr uint8_t SLIP_ESC_ESC = 0xDD;    /**< Escaped escape*/
}

namespace esp_link
{

const esp_link::packet* client::receive(uint32_t timeout)
{
    while (timeout--)
    {
        auto p = try_receive();
        if (p) return p;
    }

    return nullptr;
}

const packet* client::try_receive()
{
    while (m_uart->data_available())
    {
        uint8_t lastByte = m_uart->read();
        if (lastByte == SLIP_ESC)
        {
            m_last_was_esc = true;
            continue;
        }

        if (lastByte == SLIP_END)
        {
            auto packet = decode_packet( m_buffer, m_buffer_index);
            m_buffer_index = 0;
            m_last_was_esc = false;
            return packet;
        }

        if (m_last_was_esc)
        {
            m_last_was_esc = false;
            if (lastByte == SLIP_ESC_ESC)
            {
                lastByte = SLIP_ESC;
            }
            else if (lastByte == SLIP_ESC_END)
            {
                lastByte = SLIP_END;
            }
        }

        if (m_buffer_index <= buffer_size)
        {
            m_buffer[m_buffer_index++] = lastByte;
        }
    }
    return nullptr;
}

void client::send(const char* str)
{
    while (*str)
        send_byte( static_cast<uint8_t>( *str++));
}

bool client::sync()
{
    const packet* p = nullptr;

    // never recurse
    if (!m_syncing)
    {
        send( "sync\n");
        m_syncing = true;
        clear_input();
        send_direct( SLIP_END);
        clear_input();
        execute( esp_link::sync);
        while ((p = receive()))
        {
            if (p->cmd ==  commands::CMD_RESP_V)
            {
                m_syncing = false;
                return true;
            }
        }

        m_syncing = false;
    }
    return false;
}

const esp_link::packet* client::decode_packet(
        const uint8_t*  buffer,
        uint8_t         size)
{
    auto p = check_packet( buffer, size);
    if ( p && p->cmd == commands::CMD_SYNC)
    {
        sync();
        return nullptr;
    }
    else
    {
        return p;
    }
}

void client::log_packet(const esp_link::packet *p)
{
    char buffer[10];
    if (!p)
    {
        send( "Null\n");
    }
    else
    {
        send( "command: " );
        send( itoa( p->cmd, buffer, 10));
        send( " value: ");
        send( itoa( p->value, buffer, 10));
        send( "\n");
    }
}

const esp_link::packet* client::check_packet(
        const uint8_t*  buffer,
        uint8_t         size)
{

    if (size < 8) return nullptr;

    uint16_t crc = 0;
    const uint8_t *data = buffer;

    while (size-- > 2)
    {
        crc16_add( *data++, crc);
    }
    if (*reinterpret_cast<const uint16_t*>( data) != crc)
    {
        send("check failed\n");
        return nullptr;
    }
    else
    {
        send("got packet\n");
        return reinterpret_cast<const packet*>( buffer);
    }
}

void client::send_bytes(const uint8_t* buffer, uint8_t size)
{
    while (size)
    {
        crc16_add( *buffer, m_runningCrc);
        send_byte( *buffer);
        --size;
        ++buffer;
    }
}

void client::clear_input()
{
    while (m_uart->data_available())
        m_uart->get();
}

bool client::receive_byte(uint8_t& value, uint32_t timeout) ///< timeout in units of approx. 1.25 us
{
    while (--timeout && !m_uart->data_available()) /* spinlock */;
    if (timeout == 0) return false;

    value = m_uart->get();

    if (value == SLIP_ESC)
    {
        while (--timeout && !m_uart->data_available()) /* spinlock */;
        if (timeout == 0) return false;

        value = m_uart->get();
        if (value == SLIP_ESC_END) value = SLIP_END;
        if (value == SLIP_ESC_ESC) value = SLIP_ESC;
    }

    return true;
}

uint8_t client::receive_byte_w()
{
    uint8_t result = 0;
    while (!receive_byte( result))
        /* wait */
        ;

    return result;
}

void client::send_hex( uint8_t value)
{
    constexpr char digits[] = {
            '0', '1', '2', '3',
            '4', '5', '6', '7',
            '8', '9', 'a', 'b',
            'c', 'd', 'e', 'f',
    };
    m_uart->send( (uint8_t)digits[value / 16]);
    m_uart->send( (uint8_t)digits[value % 16]);
    m_uart->send( (uint8_t)' ');
}

void client::send_direct(uint8_t value)
{
    //send_hex( value);
    m_uart->send( value);
}

void client::send_byte(uint8_t value)
{
    switch (value)
    {
    case SLIP_END:
        send_direct( SLIP_ESC);
        send_direct( SLIP_ESC_END);
        break;
    case SLIP_ESC:
        send_direct( SLIP_ESC);
        send_direct( SLIP_ESC_ESC);
        break;
    default:
        send_direct( value);
    }
}

void client::crc16_add(uint8_t value, uint16_t &accumulator)
{
    accumulator ^= value;
    accumulator = (accumulator >> 8) | (accumulator << 8);
    accumulator ^= (accumulator & 0xff00) << 4;
    accumulator ^= (accumulator >> 8) >> 4;
    accumulator ^= (accumulator & 0xff00) >> 5;
}

void client::send_request_header(uint16_t command, uint32_t value,
        uint16_t argcount)
{
    //clear_input();
    send_direct( SLIP_END);
    m_runningCrc = 0;
    send_binary( command);
    send_binary( argcount);
    send_binary( value);
}

void client::finalize_request()
{
    // make a copy of the running crc because
    // send_binary() will change it.
    auto crc = m_runningCrc;
    send_binary( crc);
    send_direct( SLIP_END);
}


void client::add_parameter_bytes(const uint8_t* data, uint16_t length)
{
    send_binary( length);
    send_bytes( data, length);
    uint8_t pad = (4 - (length & 3)) & 3;
    while (pad--)
    {
        crc16_add( 0, m_runningCrc);
        send_direct( 0);
    }
}

}
