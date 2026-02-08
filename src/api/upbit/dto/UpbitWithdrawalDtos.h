#pragma once

#include <string>
#include <vector>
#include <optional>

// Upbit 출금 관련 JSON 데이터를 그대로 받는 구조체(Dto) 정의

namespace api::upbit
{
	// Withdrawal

	// 출금 가능 정보 조회
	struct WithdrawalPossibleResponseDto
	{
		struct Security_Level
		{
			int					security_level;				// 보안 등급
			int					fee_level;					// 수수료 등급
			bool				email_verified;				// 이메일 인증 여부
			bool				identity_auth_verified;		// 실명 인증 여부
			bool				bank_account_verified;		// 계좌 인증 여부
			bool				two_factor_auth_verified;	// 2FA 활성화 여부
			bool				locked;						// 계정 보호 상태
			bool				wallet_locked;				// 출금 보호 상태
		};
		struct Currency_Info
		{
			std::string					code;				// 통화 코드
			std::string					withdraw_fee;		// 출금 수수료
			bool						is_coin;			// 디지털 자산 여부
			std::string					wallet_state;		// 자산별 입출금 지원 이력 여부
			std::vector<std::string>	wallet_support;		// 해당 통화의 입출금 가능 여부
		};

		Security_Level		member_level;
		Currency_Info		currency;
	};

	// 계정에 등록된 출금 허용 주소 목록 조회
	struct WithdrawalAllowedAddrListResponseDto
	{
		struct WithdrawalAllowedAddr
		{
			std::string			currency;			// 출금하고자 하는 디지털 자산의 통화 코드
			std::string			net_type;			// 출금 네트워크 유형("ETH", "TRX")
			std::string			network_name;		// 출금 네트워크 이름
			std::string			withdraw_address;	// 디지털 자산 출금 시 수신 주소(등록된 주소만 사용 가능)
			std::string			withdraw_address;	// 디지털 자산 출금 시 수신 주소(등록된 주소만 사용 가능)

			std::optional<std::string>		secondary_address;			// 2차 출금 주소 (일부 디지털 자산용)
			std::optional<std::string>		beneficiary_name;			// 수취 지갑 소유주명, 개인이면 null
			std::optional<std::string>		beneficiary_company_name;	// 출금 받을 법인 명, 개인이면 null
			std::optional<std::string>		beneficiary_type;			// 계정주 타입 (개인, 법인)
			std::optional<std::string>		exchange_name;				// 출금 허용 주소가 등록된 거래소명 (바이낸스, 바이비트)
			std::optional<std::string>		wallet_type;				// 개인 지갑 종류.
		};

		std::vector<WithdrawalAllowedAddr>	addr_list;
	};

	// 디지털 자산 출금 요청 POST
	struct WithdrawalRequestDto
	{
		std::string			currency;		// 출금하고자 하는 디지털 자산의 통화 코드
		std::string			net_type;		// 출금 네트워크 유형("ETH", "TRX")                                                              
		std::string			amount;			// 출금하고자 하는 자산의 수량
		std::string			address;		// 디지털 자산 출금 시 수신 주소

		std::vector<std::string>	secondary_address;	// 디지털 자산 출금 시 수신 주소
		std::vector<std::string>	transaction_type;	// 출금 유형 (일반, 바로)

	};
	// 디지털 자산 출금 요청 GET
	struct WithdrawalResponseDto
	{

	};
}