#pragma once

#include <string>
#include <vector>
#include <optional>
#include <json.hpp>

// Upbit 내 자산과 주문 관련 JSON 데이터를 그대로 받는 구조체(Dto) 정의

namespace api::upbit::dto
{
	// Asset 
	// 계정 잔고 조회
	struct AccountDto
	{
		std::string			currency;				// 조회하고자 하는 통화 코드
		std::string			balance;				// 주문 가능 수량 또는 금액(코인은 수량, 통화는 금액)
		std::string			locked;					// 출금이나 주문 등에 잠겨 있는 잔액
		std::string			avg_buy_price;			// 매수 평균가
		bool				avg_buy_price_modified; // 매수 평균가 수정 여부
		std::string			unit_currency;			// 평균가 기준 통화 (avg_buy_price가 기준하는 단위)
	};

	inline void from_json(const nlohmann::json& j, AccountDto& o) {
		j.at("currency").get_to(o.currency);
		j.at("balance").get_to(o.balance);
		j.at("locked").get_to(o.locked);
		j.at("avg_buy_price").get_to(o.avg_buy_price);
		j.at("avg_buy_price_modified").get_to(o.avg_buy_price_modified);
		j.at("unit_currency").get_to(o.unit_currency);
	}

	struct AccountsDto
	{
		std::vector<AccountDto>		account_list;
	};

	inline void from_json(const nlohmann::json& j, AccountsDto& o) {
		// j는 array
		o.account_list = j.get<std::vector<AccountDto>>();
	}

	

	

	// Order 
	// Order를 위한 enum class
	enum class Side {
		bid, // 매수
		ask  // 매도
	};
	enum class OrdType {
		limit,      // "limit"
		price,      // "price"  (시장가 매수)
		market,     // "market" (시장가 매도)
		best,       // 최유리 지정가 같은 타입들
		// 필요하면 추가...
	};
	enum class OrdCondition										// 주문 체결 조건
	{
		ioc,		// 지정가 조건으로 즉시 체결 가능한 수량만 부분 체결하고, 잔여 수량은 취소
		fok,		// 지정가 조건으로 주문량 전량 체결 가능할 때만 주문을 실행하고, 아닌 경우 전량 주문 취소
		post_only	// 메이커(maker)주문으로 생성될 수 있는 상황에서만 주문이 생성되며 테이커(taker) 주문으로 체결되는 것을 방지
	};
	enum class SMP									// 자전 거래 체결 방지 옵션
	{
		cancel_maker,			// 메이커 주문을 취소	
		cancel_taker,			// 테이커 주문을 취소
		reduce					// 새로운 주문 생성 시 자전 거래 조건이 성립하는 경우 기존 주문과 신규 주문의 주문 수량을 줄여 체결을 방지합니다. 잔량이 0인 경우 주문을 취소
	};
	enum class OrdState
	{
		wait,		// 체결 대기
		watch,		// 예약 주문 대기
		done,		// 체결 완료
		cancel		// 주문 취소
	};
	// --- enum 문자열 매핑 추가 ---
	// nlohmann::json이 "bid"/"ask" 같은 문자열을 enum으로 변환할 수 있게 해준다.
	NLOHMANN_JSON_SERIALIZE_ENUM(Side, {
		{Side::bid, "bid"},
		{Side::ask, "ask"},
	})

	NLOHMANN_JSON_SERIALIZE_ENUM(OrdState, {
		{OrdState::wait,   "wait"},
		{OrdState::watch,  "watch"},
		{OrdState::done,   "done"},
		{OrdState::cancel, "cancel"},
	})

	NLOHMANN_JSON_SERIALIZE_ENUM(OrdCondition, {
		{OrdCondition::ioc,       "ioc"},
		{OrdCondition::fok,       "fok"},
		{OrdCondition::post_only, "post_only"},
	})

	NLOHMANN_JSON_SERIALIZE_ENUM(SMP, {
		{SMP::cancel_maker, "cancel_maker"},
		{SMP::cancel_taker, "cancel_taker"},
		{SMP::reduce,       "reduce"},
	})

	// 지정한 페어의 주문 가능 정보 조회
	struct OrderInfosDto
	{
		struct OrderInfoDto
		{
			std::string			bid_fee;				// 매수 시 적용 수수료율
			std::string			ask_fee;				// 매도 시 적용 수수료율
			std::string			maker_bid_fee;			// 매수 maker 주문 수수료율
			std::string			maker_ask_fee;			// 매도 maker 주문 수수료율

			struct MARKET_OBJECT
			{
				std::string		id;						// 페어(거래쌍)의 코드
				std::string		name;					// 페어 코드 ex) BTC/KRW

				// string enum type
				std::string		order_sides;			// 지원하는 주문 방향 (매수/매도)
				std::string		bid_sides;				// 지원하는 매수 주문 유형
				std::string		ask_sides;				// 지원하는 매도 주문 유형

				struct BID_OBJECT						// 매수 제약 조건
				{
					std::string	currency;				// 자산 구매에 사용되는 통화
					std::string	min_total;				// 매수 시 최소 주문 금액 ("5000" == 5000 KRW)
				};
				struct ASK_OBJECT						// 매도 제약 조건
				{
					std::string	currency;				// 매도 자산 통화
					std::string	min_total;				// 매도 시 최소 주문 금액
				};

				BID_OBJECT		bid;					// 매수 제약 조건
				ASK_OBJECT		ask;					// 매수 제약 조건

				std::string		max_total;				// 최대 주문 가능 금액
				std::string		state;					// 페어 운영 상태 (active)
			};
			MARKET_OBJECT		market;

			struct BID_ACCOUNT_OBJECT
			{
				std::string		currency;				// 조회하고자 하는 통화 코드
				std::string		balance;				// 주문 가능 수량 or 금액
				std::string		locked;					// 출금이나 주문 등에 잠겨 있는 잔액
				std::string		avg_buy_price;			// 매수 평균가
				bool			avg_buy_price_modified; // 매수 평균가 수정 여부
				std::string		unit_currency;			// 평균가 기준 통화. "avg_buy_price"가 기준하는 단위 (필수 x)
			};
			BID_ACCOUNT_OBJECT	bid_account;			// 호가 자산 계좌 정보

			struct ASK_ACCOUNT_OBJECT
			{
				std::string		currency;
				std::string		balance;
				std::string		locked;
				std::string		avg_buy_price;
				bool			avg_buy_price_modified;
				std::string		unit_currency;
			};
			ASK_ACCOUNT_OBJECT	ask_account;			// 기준 자산 계좌 정보
		};

		std::vector<OrderInfoDto> order_info;
		
	};

	// 주문 생성 POST (테스트는 형식은 같고 url 형태만 다름)
	struct CreateOrderRequestDto
	{
		std::string						market;					// 주문을 생성하고자 하는 대상 페어(거래쌍)
		Side							side;					// 주문 방향
		OrdType							ord_type;				// 주문 유형
		std::string						price;					// 주문 단가 or 총액

		std::optional<std::string>		volume;					// 주문 수량
		std::optional<OrdCondition>		time_in_force;			// 주문 체결 조건
		std::optional<SMP>				smp_type;				// 자전 거래 체결 방지 옵션

		std::string						identifier;				// 클라이언트 지정 주문 식별자
	};
	// 주문 생성 GET
	struct CreateOrderResponseDto
	{
		std::string						market;					// 페어(거래쌍)의 코드
		std::string						uuid;					// 주문의 유일 식별자

		Side							side;					// 주문 방향(매수/매도)
		std::string						ord_type;				// 주문 유형
		std::optional<std::string>		price;					// 주문 단가 또는 총액
		std::optional<std::string>		volume;					// 주문 요청 수량
		OrdState						state;					// 주문 상태
		std::string						created_at;				// 주문 생성 시각 (KST 기준)

		std::string						remaining_volume;		// 체결 후 남은 주문 양
		std::string						executed_volume;		// 체결된 양
		std::string						reserved_fee;			// 수수료로 예약된 비용
		std::string						remaining_fee;			// 남은 수수료
		std::string						paid_fee;				// 사용된 수수료
		std::string						locked;					// 거래에 사용 중인 비용

		std::optional<OrdCondition>		time_in_force;			// 주문 체결 조건
		std::optional<SMP>				smp_type;				// 자전 거래 체결 방지 옵션

		std::string						prevented_volume;		// 자전거래 방지로 인해 취소된 수량
		std::string						prevented_locked;		// 자전거래 방지로 인해 해제된 자산

		int								trades_count{ 0 };			// 해당 주문에 대한 체결 건수
		std::string						identifier;				// 클라이언트 지정 주문 식별자
	};

	// 개별 주문 조회 (조회 시 uuid 또는 identifier 중 하나는 반드시 파라미터로 포함해야 조회됨
	struct OrderResponseDto
	{
		std::string						market;					// 페어(거래쌍)의 코드
		std::string						uuid;					// 주문의 유일 식별자

		Side							side;					// 주문 방향(매수/매도)
		std::string						ord_type;				// 주문 유형
		std::optional<std::string>		price;					// 주문 단가 또는 총액
		std::optional<std::string>		volume;					// 주문 요청 수량
		OrdState						state;					// 주문 상태
		std::string						created_at;				// 주문 생성 시각 (KST 기준)

		std::optional<std::string>		remaining_volume;		// 체결 후 남은 주문 양
		std::string						executed_volume;		// 체결된 양
		std::string						reserved_fee;			// 수수료로 예약된 비용
		std::string						remaining_fee;			// 남은 수수료
		std::string						paid_fee;				// 사용된 수수료
		std::string						locked;					// 거래에 사용 중인 비용

		std::optional<OrdCondition>		time_in_force;			// 주문 체결 조건
		std::optional<SMP>				smp_type;				// 자전 거래 체결 방지 옵션

		std::string						prevented_volume;		// 자전거래 방지로 인해 취소된 수량
		std::optional<std::string>		prevented_locked;		// 자전거래 방지로 인해 해제된 자산

		int								trades_count{ 0 };			// 해당 주문에 대한 체결 건수

		struct ArrayOfTrade
		{
			std::string					market;					// 페어(거래쌍)의 코드
			std::string					uuid;					// 주문의 유일 식별자
			std::string					price;					// 체결 단가
			std::string					volume;					// 체결 수량
			std::string					funds;					// 체결 총액
			std::string					trend;					// 체결 시세 흐름("up" - 매수에 의한 체결)
			std::string					created_at;				// 주문 생성 시각 (KST 기준)
			Side						side;					// 주문 방향(매수/매도)

		};
		ArrayOfTrade					trades;					// 주문의 체결 목록
	};
	// 개별 주문 목록 조회 (조회 시 uuid 또는 identifier 중 하나는 반드시 파라미터로 포함해야 조회됨
	struct OrdersResponseDto
	{
		std::vector<OrderResponseDto>	orders_response;
	};

	// 체결 대기 중인 주문 목록 조회 (조회 시 state와 state[]는 동시 사용 불가)
	struct WaitOrderResponseDto
	{
		std::string						market;					// 페어(거래쌍)의 코드
		std::string						uuid;					// 주문의 유일 식별자

		Side							side;					// 주문 방향(매수/매도)
		std::string						ord_type;				// 주문 유형
		std::optional<std::string>		price;					// 주문 단가 또는 총액
		std::optional<std::string>		volume;					// 주문 요청 수량
		OrdState						state;					// 주문 상태
		std::string						created_at;				// 주문 생성 시각 (KST 기준)

		std::string						remaining_volume;		// 체결 후 남은 주문 양
		std::string						executed_volume;		// 체결된 양
		std::string						executed_funds;			// 현재까지 체결된 금액

		std::string						reserved_fee;			// 수수료로 예약된 비용
		std::string						remaining_fee;			// 남은 수수료
		std::string						paid_fee;				// 사용된 수수료
		std::string						locked;					// 거래에 사용 중인 비용

		std::optional<OrdCondition>		time_in_force;			// 주문 체결 조건
		std::optional<SMP>				smp_type;				// 자전 거래 체결 방지 옵션

		std::string						prevented_volume;		// 자전거래 방지로 인해 취소된 수량
		std::optional<std::string>		prevented_locked;		// 자전거래 방지로 인해 해제된 자산

		int								trades_count{ 0 };		// 해당 주문에 대한 체결 건수
		std::optional<std::string>		identifier;				// 주문 생성시 클라이언트가 지정한 주문 식별자
	};

	inline void from_json(const nlohmann::json& j, WaitOrderResponseDto& o)
	{
		j.at("market").get_to(o.market);
		j.at("uuid").get_to(o.uuid);

		j.at("side").get_to(o.side);
		j.at("ord_type").get_to(o.ord_type);

		// optional string: null 가능성 방어
		if (j.contains("price") && !j.at("price").is_null()) j.at("price").get_to(o.price);
		else o.price.reset();

		if (j.contains("volume") && !j.at("volume").is_null()) j.at("volume").get_to(o.volume);
		else o.volume.reset();

		j.at("state").get_to(o.state);
		j.at("created_at").get_to(o.created_at);

		j.at("remaining_volume").get_to(o.remaining_volume);
		j.at("executed_volume").get_to(o.executed_volume);

		// executed_funds는 스펙상 존재(없을 수도 있으니 방어)
		if (j.contains("executed_funds") && !j.at("executed_funds").is_null())
			j.at("executed_funds").get_to(o.executed_funds);
		else
			o.executed_funds = "0";

		j.at("reserved_fee").get_to(o.reserved_fee);
		j.at("locked").get_to(o.locked);

		// ★ 누락 보완: remaining_fee / paid_fee
		if (j.contains("remaining_fee") && !j.at("remaining_fee").is_null())
			j.at("remaining_fee").get_to(o.remaining_fee);
		else
			o.remaining_fee = "0";

		if (j.contains("paid_fee") && !j.at("paid_fee").is_null())
			j.at("paid_fee").get_to(o.paid_fee);
		else
			o.paid_fee = "0";

		// ★ 누락 보완: time_in_force / smp_type (optional enum)
		if (j.contains("time_in_force") && !j.at("time_in_force").is_null())
			j.at("time_in_force").get_to(o.time_in_force);
		else
			o.time_in_force.reset();

		if (j.contains("smp_type") && !j.at("smp_type").is_null())
			j.at("smp_type").get_to(o.smp_type);
		else
			o.smp_type.reset();

		// ★ 누락 보완: prevented_* (optional 포함)
		if (j.contains("prevented_volume") && !j.at("prevented_volume").is_null())
			j.at("prevented_volume").get_to(o.prevented_volume);
		else
			o.prevented_volume = "0";

		if (j.contains("prevented_locked") && !j.at("prevented_locked").is_null())
			j.at("prevented_locked").get_to(o.prevented_locked);
		else
			o.prevented_locked.reset();

		j.at("trades_count").get_to(o.trades_count);

		if (j.contains("identifier") && !j.at("identifier").is_null())
			j.at("identifier").get_to(o.identifier);
		else
			o.identifier.reset();
	}
	struct WaitOrdersResponseDto
	{
		std::vector<WaitOrderResponseDto>	wait_order_list;
	};
	inline void from_json(const nlohmann::json& j, WaitOrdersResponseDto& o) {
		// j는 array
		o.wait_order_list = j.get<std::vector<WaitOrderResponseDto>>();
	}



	// 종료 주문 목록 조회 (조회 시 state와 state[]는 동시 사용 불가)
	struct ClosedOrdersResponseDto
	{
		struct ClosedOrderResponseDto
		{
			std::string						market;					// 페어(거래쌍)의 코드
			std::string						uuid;					// 주문의 유일 식별자

			Side							side;					// 주문 방향(매수/매도)
			std::string						ord_type;				// 주문 유형
			std::string						price;					// 주문 단가 또는 총액
			std::string						volume;					// 주문 요청 수량
			OrdState						state;					// 주문 상태
			std::string						created_at;				// 주문 생성 시각 (KST 기준)

			std::string						remaining_volume;		// 체결 후 남은 주문 양
			std::string						executed_volume;		// 체결된 양
			std::string						executed_funds;			// 현재까지 체결된 금액

			std::string						reserved_fee;			// 수수료로 예약된 비용
			std::string						remaining_fee;			// 남은 수수료
			std::string						paid_fee;				// 사용된 수수료
			std::string						locked;					// 거래에 사용 중인 비용

			std::optional<OrdCondition>		time_in_force;			// 주문 체결 조건

			std::string						prevented_volume;		// 자전거래 방지로 인해 취소된 수량
			std::string						prevented_locked;		// 자전거래 방지로 인해 해제된 자산

			int								trades_count{ 0 };			// 해당 주문에 대한 체결 건수
			std::optional<std::string>		identifier;				// 주문 생성시 클라이언트가 지정한 주문 식별자
		};
		
		std::vector<ClosedOrderResponseDto>	closed_order_list;
	};

	// 개별 주문 취소 접수
		// 이 경우 자주 사용하면 쿼리용 Request Dto를 따로 만들자
	struct CancelOrdersResponseDto
	{
		struct CancelOrderResponseDto
		{
			std::string						market;					// 페어(거래쌍)의 코드
			std::string						uuid;					// 주문의 유일 식별자

			Side							side;					// 주문 방향(매수/매도)
			std::string						ord_type;				// 주문 유형
			std::optional<std::string>		price;					// 주문 단가 또는 총액
			std::optional<std::string>		volume;					// 주문 요청 수량
			OrdState						state;					// 주문 상태
			std::string						created_at;				// 주문 생성 시각 (KST 기준)

			std::string						remaining_volume;		// 체결 후 남은 주문 양
			std::string						executed_volume;		// 체결된 양

			std::string						reserved_fee;			// 수수료로 예약된 비용
			std::string						remaining_fee;			// 남은 수수료
			std::string						paid_fee;				// 사용된 수수료
			std::string						locked;					// 거래에 사용 중인 비용

			std::optional<OrdCondition>		time_in_force;			// 주문 체결 조건

			std::string						prevented_volume;		// 자전거래 방지로 인해 취소된 수량
			std::string						prevented_locked;		// 자전거래 방지로 인해 해제된 자산

			int								trades_count{ 0 };			// 해당 주문에 대한 체결 건수
			std::optional<std::string>		identifier;				// 주문 생성시 클라이언트가 지정한 주문 식별자
		};

		std::vector<CancelOrderResponseDto>		cancel_order_list;
	};
	// 주문 목록 취소 접수 (한 번에 최대 20개) + 일괄 취소도 처리됨
	struct CancelOrderListResponseDto
	{
		struct CancelledOrderDto
		{
			std::string              uuid;       // 주문 유일 식별자
			std::string              market;     // "KRW-BTC"
			std::optional<std::string> identifier; // 주문 생성 시 지정한 식별자(없을 수도 있음)
		};
		struct CancelOrdersSuccessDto
		{
			int                               count{ 0 };	// 성공적으로 취소된 주문 수
			std::vector<CancelledOrderDto>    orders;		// 취소된 주문 목록
		};
		struct CancelOrdersFailedDto
		{
			int                               count{ 0 };	// 취소 실패한 주문 수
			std::vector<CancelledOrderDto>    orders;		// 취소 실패한 주문 목록
		};

		CancelOrdersSuccessDto		success;
		CancelOrdersFailedDto		failed;
	};

	// 취소 후 재주문 POST
	struct CancelAndOrderRequestDto
	{
		std::optional<std::string>		prev_order_uuid;			// 취소하고자 하는 주문의 유일식별자(UUID)
		std::optional<std::string>		prev_order_identifier;		// 취소하고자 하는 주문의 유일식별자(UUID)

		OrdType							new_ord_type;				// 신규 주문의 주문 유형
		std::optional<std::string>		new_volume;					// 신규 주문 요청 수량
		std::optional<std::string>		price;						// 신규 주문 단가 또는 총액

		std::optional<OrdCondition>		new_time_in_force;			// 신규 주문 체결 조건
		std::optional<SMP>				new_smp_type;				// 신규 자전 거래 체결 방지 옵션


		std::optional<std::string>		identifier;					// 신규 주문 생성시 클라이언트가 지정한 주문 식별자
	};
	// 취소 후 재주문 GET
	struct CancelAndOrderResponseDto
	{
		std::string						market;					// 페어(거래쌍)의 코드
		std::string						uuid;					// 주문의 유일 식별자

		Side							side;					// 주문 방향(매수/매도)
		std::string						ord_type;				// 주문 유형
		std::optional<std::string>		price;					// 주문 단가 또는 총액
		std::optional<std::string>		volume;					// 주문 요청 수량
		OrdState						state;					// 주문 상태
		std::string						created_at;				// 주문 생성 시각 (KST 기준)

		std::string						remaining_volume;		// 체결 후 남은 주문 양
		std::string						executed_volume;		// 체결된 양

		std::string						reserved_fee;			// 수수료로 예약된 비용
		std::string						remaining_fee;			// 남은 수수료
		std::string						paid_fee;				// 사용된 수수료
		std::string						locked;					// 거래에 사용 중인 비용

		std::optional<OrdCondition>		time_in_force;			// 주문 체결 조건
		std::optional<SMP>				smp_type;				// 자전 거래 체결 방지 옵션

		std::string						prevented_volume;		// 자전거래 방지로 인해 취소된 수량
		std::string						prevented_locked;		// 자전거래 방지로 인해 해제된 자산

		int								trades_count{ 0 };		// 해당 주문에 대한 체결 건수
		std::optional<std::string>		identifier;				// 주문 생성시 클라이언트가 지정한 주문 식별자

		// 다른 점
		std::string						new_uuid;				// 신규 주문의 유일 식별자
		std::optional<std::string>		new_identifier;			// 신규 주문 생성시 클라이언트가 지정한 주문 식별자
	};
}