#pragma once

#include <stdexcept>
#include <string>

class FECException : public std::runtime_error
{
public:
    ~FECException() = default;

    const char* what() const noexcept override
    {
        return m_what.c_str();
    }

    const std::string& GetMsg() const { return m_message; }

protected:
    FECException(const std::string& type, const std::string& message, const std::string& function)
        : std::runtime_error(function + ": " + message)
    {
        m_type = type;
        m_message = message;
        m_function = function;
        m_what = function + ": " + message;
    }

private:
    std::string m_type;
    std::string m_message;
    std::string m_function;
    std::string m_what;
};