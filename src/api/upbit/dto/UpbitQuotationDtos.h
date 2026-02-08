#pragma once

#include <string>
#include <vector>
#include <optional>
#include <json.hpp>

// Upbit 시세, 코인 정보 등의 JSON 데이터를 그대로 받는 구조체(Dto) 정의

namespace api::upbit::dto
{	
	// 업비트 마켓(종목) 정보 DTO
	struct MarketEventDto 
	{
		struct MarketCautionDto
		{
			bool	PRICE_FLUCTUATIONS;					// 가격 급등락 경보
			bool	RADING_VOLUME_SOARING;				// 거래량 급증 경보
			bool	DEPOSIT_AMOUNT_SOARING;				// 입금량 급증 경보
			bool	GLOBAL_PRICE_DIFFERENCES;			// 국내외 시세 차이 경보
			bool	CONCENTRATION_OF_SMALL_ACCOUNTS;	// 소수 계정 집중 거래 경보
		};

		bool warning = false;                         // 유의 종목 여부
		std::optional<MarketCautionDto> caution; // 주의 종목 정보 (없을 수도 있으니 optional 권장)
	};
	struct MarketDto
	{
		std::string			market;			// 마켓 코드 (ex: KRW-BTC)
		std::string			korean_name;	// 한글명
		std::string			english_name;	// 영문명

		std::optional<MarketEventDto> market_event; // 마켓 경고 정보 (없을 수도 있으니 optional 권장)
	};

	// nlohmann::json 매핑 (ADL)
	inline void from_json(const nlohmann::json& j, MarketDto& d)
	{
		j.at("market").get_to(d.market);
		j.at("korean_name").get_to(d.korean_name);
		j.at("english_name").get_to(d.english_name);
	}
	
	/*
	* 현재는 직접 종목을 골라서 시세 정보를 받는 페어 단위의 Ticker DTO만 정의
	* 추후 실시간을 거래 종목을 고르는 경우 마켓 페어 단위의 조회 DTO도 정의 필요
	*/

	// 업비트 페어 단위 시세 정보 DTO (종목은 내가 고름)
	struct TickerDto
	{
		// 기본 정보
		std::string			market;					// 마켓 코드 (ex: KRW-BTC)
		std::string			trade_date;				// 최근 체결 일자 UTC 기준 (yyyyMMdd)
		std::string			trade_time;				// 최근 체결 일자 UTC 기준	 (HHmmss)
		std::string			trade_date_kst;			// 최근 체결 일자 KST 기준	 (yyyyMMdd)
		std::string			trade_time_kst;			// 최근 체결 일자 KST 기준	 (HHmmss)
		std::int64_t		trade_timestamp;		// 체결 시각의 밀리초단위 타임스탬프

		// 가격
		double				opening_price;			// 해당 페어의 시가
		double				high_price;				// 해당 페어의 고가
		double				low_price;				// 해당 페어의 저가
		double				trade_price;			// 해당 페어의 종가(최종 체결가)
		double				prev_closing_price;		// 전일 종가 (UTC0시 기준)
		std::string			change;					// 가격 변동 상태(RISE, FALL, EVEN)
		double				change_price;			// 전일 종가 대비 가격 변화(trade_price - prev_closing_price)
		double				change_rate;			// 전일 종가 대비 가격 변화율 ((trade_price - prev_closing_price) / prev_closing_price)
		double				signed_change_price;	// 부호 있는 가격 변화 (change_price와 동일하지만 부호 존재)
		double				signed_change_rate;		// 부호 있는 가격 변화율 (change_rate와 동일하지만 부호 존재)

		// 거래량
		double				trade_volume;			// 해당 페어의 최근 거래량
		double				acc_trade_price;		// 해당 페어의 누적 거래대금 (UTC 0시부터 누적)
		double				acc_trade_price_24h;	// 해당 페어의 24시간 누적 거래대금
		double				acc_trade_volume;		// 해당 페어의 누적 거래량 (UTC 0시부터 누적)
		double				acc_trade_volume_24h;	// 해당 페어의 24시간 누적 거래량

		// 최고가 / 최저가
		double				highest_52_week_price;	// 최근 52주 최고가
		std::string			highest_52_week_date;	// 최근 52주 최고가 달성일자 (yyyy-MM-dd)
		double				lowest_52_week_price;	// 최근 52주 최저가
		std::string			lowest_52_week_date;	// 최근 52주 최저가 달성일자 (yyyy-MM-dd)
		int					timestamp;				// 타임스탬프
	};
	// nlohmann::json 매핑 (ADL)
	inline void from_json(const nlohmann::json& j, TickerDto& d)
	{
		j.at("market").get_to(d.market);

		j.at("opening_price").get_to(d.opening_price);
		j.at("high_price").get_to(d.high_price);
		j.at("low_price").get_to(d.low_price);
		j.at("trade_price").get_to(d.trade_price);

		j.at("prev_closing_price").get_to(d.prev_closing_price);
		j.at("trade_timestamp").get_to(d.trade_timestamp);

		j.at("signed_change_price").get_to(d.signed_change_price);
		j.at("signed_change_rate").get_to(d.signed_change_rate);

		j.at("trade_volume").get_to(d.trade_volume);
		j.at("acc_trade_volume").get_to(d.acc_trade_volume);
		j.at("acc_trade_volume_24h").get_to(d.acc_trade_volume_24h);
	}


	// 지정한 페어의 캔들(봉) 데이터 DTO
	struct CandleDto_Minute
	{
		// 기본 정보
		std::string		market;						// 마켓 코드 (ex: KRW-BTC)
		std::string		candle_date_time_utc;		// 캔들 기준 시작 시각(UTC)
		std::string		candle_date_time_kst;		// 캔들 기준 시작 시각(KST)
		
		// 가격
		double			opening_price;				// 해당 캔들의 시가
		double			high_price;					// 해당 캔들의 고가
		double			low_price;					// 해당 캔들의 저가
		double			trade_price;				// 해당 캔들의 종가
		std::int64_t	timestamp;					// 캔들의 마지막 틱의 타임스탬프

		// 거래량
		double			candle_acc_trade_price;		// 해당 캔들의 누적 거래대금
		double			candle_acc_trade_volume;	// 해당 캔들의 누적 거래량

		// 분 단위 - 1, 3, 5, 10, 15, 30, 60, 240 (우리는 3분부터 사용)
		int				unit;						// 캔들 단위 (분)
	};
	inline void from_json(const nlohmann::json& j, CandleDto_Minute& d)
	{
		if (j.contains("market")) {
			d.market = j.at("market").get<std::string>();
		}
		else if (j.contains("code")) {
			d.market = j.at("code").get<std::string>();
		}

		j.at("opening_price").get_to(d.opening_price);
		j.at("high_price").get_to(d.high_price);
		j.at("low_price").get_to(d.low_price);
		j.at("trade_price").get_to(d.trade_price);

		j.at("candle_acc_trade_volume").get_to(d.candle_acc_trade_volume);

		j.at("candle_date_time_kst").get_to(d.candle_date_time_kst);
	}
	struct CandleDto_Day
	{
		// 기본 정보
		std::string		market;						// 마켓 코드 (ex: KRW-BTC)
		std::string		candle_date_time_utc;		// 캔들 기준 시작 시각(UTC)
		std::string		candle_date_time_kst;		// 캔들 기준 시작 시각(KST)

		// 가격
		double			opening_price;				// 해당 캔들의 시가
		double			high_price;					// 해당 캔들의 고가
		double			low_price;					// 해당 캔들의 저가
		double			trade_price;				// 해당 캔들의 종가
		std::int64_t	timestamp;					// 캔들의 마지막 틱의 타임스탬프

		// 거래량
		double			candle_acc_trade_price;		// 해당 캔들의 누적 거래대금
		double			candle_acc_trade_volume;	// 해당 캔들의 누적 거래량
		
		// 전일	종가 관련 정보
		double			prev_closing_price;			// 전일 종가
		double			change_price;				// 전일 종가 대비 가격 변화(trade_price - prev_closing_price)
		double			change_rate;				// 전일 종가 대비 가격 변화율 ((trade_price - prev_closing_price) / prev_closing_price)
		double			coverted_trade_price;		// 종가 환산 가격(converted_trade_price에 요청된 통화로 환산된 가격)
	};
	struct CandleDto_Week
	{
		// 기본 정보
		std::string		market;						// 마켓 코드 (ex: KRW-BTC)
		std::string		candle_date_time_utc;		// 캔들 기준 시작 시각(UTC)
		std::string		candle_date_time_kst;		// 캔들 기준 시작 시각(KST)

		// 가격
		double			opening_price;				// 해당 캔들의 시가
		double			high_price;					// 해당 캔들의 고가
		double			low_price;					// 해당 캔들의 저가
		double			trade_price;				// 해당 캔들의 종가
		std::int64_t	timestamp;					// 캔들의 마지막 틱의 타임스탬프

		// 거래량
		double			candle_acc_trade_price;		// 해당 캔들의 누적 거래대금
		double			candle_acc_trade_volume;	// 해당 캔들의 누적 거래량

		// 부가 정보
		std::string		first_day_of_period;		// 해당 주 캔들 집계 시작일자 (yyyy-MM-dd)
	};
	struct CandleDto_Month
	{
		// 기본 정보
		std::string		market;						// 마켓 코드 (ex: KRW-BTC)
		std::string		candle_date_time_utc;		// 캔들 기준 시작 시각(UTC)
		std::string		candle_date_time_kst;		// 캔들 기준 시작 시각(KST)

		// 가격
		double			opening_price;				// 해당 캔들의 시가
		double			high_price;					// 해당 캔들의 고가
		double			low_price;					// 해당 캔들의 저가
		double			trade_price;				// 해당 캔들의 종가
		int				timestamp;					// 캔들의 마지막 틱의 타임스탬프

		// 거래량
		double			candle_acc_trade_price;		// 해당 캔들의 누적 거래대금
		double			candle_acc_trade_volume;	// 해당 캔들의 누적 거래량

		// 부가 정보
		std::string		first_day_of_period;		// 해당 월 캔들 집계 시작일자 (yyyy-MM-dd)
	};

	// 지정한 페어의 최근 체결 목록
	struct TradeDto
	{
		// 기본 정보
		std::string		market;			// 마켓 코드 (ex: KRW-BTC)
		std::string		trade_date_utc;	// 체결 일자 UTC 기준 (yyyyMMdd)
		std::string		trade_time_utc;	// 체결 시각 UTC 기준 (HHmmss)
		std::int64_t	timestamp;		// 체결 시각의 밀리초단위 타임스탬프

		// 체결 정보
		double			trade_price;	// 체결 가격
		double			trade_volume;	// 체결 수량

		// 부가 정보
		double			prev_closing_price;	// 전일 종가 (UTC0시 기준)
		double 			change_price;		// 전일 종가 대비 가격 변화(trade_price - prev_closing_price)
		std::string		ask_bid;			// 매도/매수 구분 (ASK, BID)
		
		// 체결 번호
		int 			sequential_id;		// 체결 번호
	};

	// 지정한 종목들의 실시간 호가 정보 (원화 마켓에서만 제공)
	struct OrderbookDto
	{
		// 기본 정보
		std::string		market;			// 마켓 코드 (ex: KRW-BTC)
		std::int64_t	timestamp;		// 조회 요청 시각의 타임스탬프(ms)
		
		// 매수 / 매도 잔량
		double			total_ask_size;	// 전체 매도 잔량
		double			total_bid_size;	// 전체 매수 잔량

		// 호가 목록
		struct UpbitOrderbookUnitDto
		{
			// 매도
			double	ask_price;		// 매도 호가
			double	ask_size;		// 매도 잔량

			// 매수
			double	bid_price;		// 매수 호가
			double	bid_size;		// 매수 잔량
		};

		std::vector<UpbitOrderbookUnitDto> orderbook_units; // 호가 단위 목록 (최대 15개)
		double 		level;			// 해당 호가에 적용된 가격 단위 Default : 0
	};
	inline void from_json(const nlohmann::json& j, OrderbookDto::UpbitOrderbookUnitDto& u)
	{
		j.at("ask_price").get_to(u.ask_price);
		j.at("ask_size").get_to(u.ask_size);
		j.at("bid_price").get_to(u.bid_price);
		j.at("bid_size").get_to(u.bid_size);
	}

	inline void from_json(const nlohmann::json& j, OrderbookDto& d)
	{
		j.at("market").get_to(d.market);
		j.at("timestamp").get_to(d.timestamp);
		j.at("total_ask_size").get_to(d.total_ask_size);
		j.at("total_bid_size").get_to(d.total_bid_size);

		j.at("orderbook_units").get_to(d.orderbook_units);

		// 응답에 level이 없는 경우도 대비하고 싶으면 contains로 방어
		if (j.contains("level"))
			j.at("level").get_to(d.level);
		else
			d.level = 0.0;
	}
	struct OrderbookPolicyDto
	{
		std::string		market;				// 마켓 코드 (ex: KRW-BTC)
		std::string		quote_currency;		// 해당 페어의 마켓 통화 코드 (ex: KRW)
		std::string		orderbook_units;	// 해당 페어에 적용되는 호가 단위 
	};

}