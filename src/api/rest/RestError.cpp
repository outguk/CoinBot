#include <ostream>
#include "RestError.h"

namespace api::rest {

	std::ostream& operator<<(std::ostream& os, RestErrorCode code)
	{
		switch (code)
		{
		case RestErrorCode::ResolveFailed:     return os << "ResolveFailed";
		case RestErrorCode::ConnectFailed:     return os << "ConnectFailed";
		case RestErrorCode::HandshakeFailed:   return os << "HandshakeFailed";
		case RestErrorCode::WriteFailed:       return os << "WriteFailed";
		case RestErrorCode::ReadFailed:        return os << "ReadFailed";
		case RestErrorCode::Timeout:           return os << "Timeout";
		case RestErrorCode::BadStatus:         return os << "BadStatus";
		case RestErrorCode::InvalidArgument:   return os << "InvalidArgument";
		case RestErrorCode::ParseError:        return os << "ParseError";
		case RestErrorCode::Unknown:            return os << "Unknown";
		default:                               return os << "Unknown";
		}
	}
}
