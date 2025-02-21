#include <Processors/Formats/Impl/RawStoreInputFormat.h>

#include <DataStreams/copyData.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Processors/Formats/Impl/TabSeparatedRawRowOutputFormat.h>
#include <Processors/Formats/InputStreamFromInputFormat.h>
#include <Processors/Formats/OutputStreamToOutputFormat.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include <gtest/gtest.h>

using namespace DB;

std::pair<BlockInputStreamPtr, BlockOutputStreamPtr>
prepare(Block & sample, FormatSettings & format_, ReadBufferFromString & in, WriteBufferFromString & out)
{
    RowInputFormatParams in_params{DEFAULT_INSERT_BLOCK_SIZE, 0, 0};
    RowOutputFormatParams out_params{[](const Columns & /* columns */, size_t /* row */) {}};

    InputFormatPtr input_format = std::make_shared<RawStoreInputFormat>(in, sample, in_params, format_, false);
    BlockInputStreamPtr block_input = std::make_shared<InputStreamFromInputFormat>(std::move(input_format));

    BlockOutputStreamPtr block_output = std::make_shared<OutputStreamToOutputFormat>(
        std::make_shared<TabSeparatedRawRowOutputFormat>(out, sample, false, false, out_params, format_));
    return std::make_pair(std::move(block_input), std::move(block_output));
}

void checkOutput(FormatSettings & format_, String & in, String exp)
{
    Block sample;
    {
        ColumnWithTypeAndName col;
        col.name = "a";
        col.type = std::make_shared<DataTypeUInt64>();
        sample.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_raw";
        col.type = std::make_shared<DataTypeString>();
        sample.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_time";
        col.type = std::make_shared<DataTypeDateTime64>(DataTypeDateTime64::default_scale, String{"Asia/Shanghai"});
        sample.insert(std::move(col));
    }

    String out;
    ReadBufferFromString in_buf{in};
    WriteBufferFromString out_buf{out};
    std::pair<BlockInputStreamPtr, BlockOutputStreamPtr> streams = prepare(sample, format_, in_buf, out_buf);

    copyData(*(streams.first), *(streams.second));

    std::vector<String> rows;
    String sep = "\n";
    boost::algorithm::split(rows, out, boost::is_any_of(sep));

    sep = "\t";
    std::vector<String> cols;
    boost::algorithm::split(cols, rows[0], boost::is_any_of(sep));
    EXPECT_EQ(cols[2], exp);
}

void checkException(Block & sample, FormatSettings & format_, String & in)
{
    String out;
    ReadBufferFromString in_buf{in};
    WriteBufferFromString out_buf{out};
    std::pair<BlockInputStreamPtr, BlockOutputStreamPtr> streams = prepare(sample, format_, in_buf, out_buf);

    EXPECT_THROW(copyData(*(streams.first), *(streams.second)), DB::Exception);
}

void checkCtorException(Block & sample, FormatSettings & format_, String & in)
{
    String out;
    ReadBufferFromString in_buf(in);
    WriteBufferFromString out_buf{out};
    std::pair<BlockInputStreamPtr, BlockOutputStreamPtr> streams;

    EXPECT_THROW(streams = prepare(sample, format_, in_buf, out_buf), DB::Exception);
}

TEST(RawStoreFormatTest, JSONExtract)
{
    String str = R"###({"_raw": "{\"log\":{\"time\":\"2021-03-21 00:10:23\"}}"})###";

    FormatSettings format_settings;
    format_settings.rawstore.rawstore_time_extraction_type = "json_path";
    format_settings.rawstore.rawstore_time_extraction_rule = "log.time";

    checkOutput(format_settings, str, "2021-03-21 00:10:23.000");

    String multi_rows = "["
                        "{\"_raw\": \"{\\\"log\\\":{\\\"time\\\":\\\"2021-03-21 00:10:23\\\"}}\"},"
                        "{\"_raw\": \"{\\\"log\\\":{\\\"time\\\":\\\"2021-03-22 00:10:23\\\"}}\"}"
                        "]";
    checkOutput(format_settings, multi_rows, "2021-03-21 00:10:23.000");
}

TEST(RawStoreFormatTest, RegexExtract)
{
    String str = R"###({ "_raw": "2021-03-21 00:10:23, [Apache] This is a error."})###";

    FormatSettings format_settings;
    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(?P<_time>.+),\s+\[\w+\])";

    checkOutput(format_settings, str, "2021-03-21 00:10:23.000");
}

void checkISOFormat(std::vector<std::pair<String, String>> testcases)
{
    FormatSettings format_settings;
    format_settings.date_time_input_format = FormatSettings::DateTimeInputFormat::BestEffort;
    for (const auto & tc : testcases)
    {
        String str = "{\"_raw\": \"" + tc.first + ", [Apache] This is a error.\"}";
        checkOutput(format_settings, str, tc.second);
    }
}

TEST(RawStoreFormatTest, AutoExtractText)
{
    /// Extension format
    checkISOFormat(
        {{"2002-12-15 12:00:00.234+01:30", "2002-12-15 18:30:00.234"},
         {"2002-12-15 12:00:00.234+01:30", "2002-12-15 18:30:00.234"},
         {"2002-12-15T12:00:00.234+01:30", "2002-12-15 18:30:00.234"},
         {"2002-12-15 12:00:00.234Z", "2002-12-15 20:00:00.234"},
         {"2002-12-15T12:00:00.234Z", "2002-12-15 20:00:00.234"},
         {"2002-12-15 12:00:00.234+0130", "2002-12-15 18:30:00.234"},
         {"2002-12-15 12:00:00.234-0130", "2002-12-15 21:30:00.234"},
         {"2011-03-17T01:00:00-04:00", "2011-03-17 13:00:00.000"},
         {"2011-03-17T01:00:00+0400", "2011-03-17 05:00:00.000"},
         {"2002-12-15 12:00:00Z", "2002-12-15 20:00:00.000"},
         {"2002-12-15T12:00:00+04", "2002-12-15 16:00:00.000"},
         {"2002-12-15T12:00+08:30", "2002-12-15 11:30:00.000"},
         {"2002-12-15T12:00+0430", "2002-12-15 15:30:00.000"},
         {"2022-12-15T12+04:30", "2022-12-15 15:30:00.000"},
         {"2022-12-15 12+04:30", "2022-12-15 15:30:00.000"},
         {"2022-12-15 12Z", "2022-12-15 20:00:00.000"},
         {"2022-12-15T12Z", "2022-12-15 20:00:00.000"},
         {"2002-12-15+04:30", "2002-12-15 03:30:00.000"},
         {"2002-12-15+0430", "2002-12-15 03:30:00.000"},
         {"2002-12-15Z", "2002-12-15 08:00:00.000"}});

    /// Extension format: not match or partial match
    checkISOFormat(
        {{"2002", "1970-01-01 08:00:00.000"},
         {"2002-12-15T12:00.123+0430", "2002-12-15 12:00:00.000"},
         {"2022-12-15T12.123+04:30", "2022-12-15 12:00:00.000"},
         {"2002-12-15+", "2002-12-15 00:00:00.000"}});

    /// Basic format
    checkISOFormat({
        {"20210401T000000.123+08:00", "2021-04-01 00:00:00.123"},
        {"20210401T000000.123+0830", "2021-03-31 23:30:00.123"},
        {"20210401T000000.123-08:00", "2021-04-01 16:00:00.123"},
        {"20210401T000000.123-08", "2021-04-01 16:00:00.123"},
        {"20210401T000000.123Z", "2021-04-01 08:00:00.123"},
        {"20210401T000000.123", "2021-04-01 00:00:00.123"},
        {"20210401T000000+08:00", "2021-04-01 00:00:00.000"},
        {"20210401T000000+0830", "2021-03-31 23:30:00.000"},
        {"20210401T000000-08:00", "2021-04-01 16:00:00.000"},
        {"20210401T000000-08", "2021-04-01 16:00:00.000"},
        {"20210401T000000Z", "2021-04-01 08:00:00.000"},
        {"20210401T00+08:00", "2021-04-01 00:00:00.000"},
        {"20210401T00+0830", "2021-03-31 23:30:00.000"},
        {"20210401T00Z", "2021-04-01 08:00:00.000"},
        {"20210401T00", "2021-04-01 00:00:00.000"},
        {"20210401000000.123+08:00", "2021-04-01 00:00:00.123"},
        {"20210401000000.123+0830", "2021-03-31 23:30:00.123"},
        {"20210401000000.123-08:00", "2021-04-01 16:00:00.123"},
        {"20210401000000.123-08", "2021-04-01 16:00:00.123"},
        {"20210401000000.123Z", "2021-04-01 08:00:00.123"},
        {"20210401000000.123", "2021-04-01 00:00:00.123"},
        {"20210401000000+08:00", "2021-04-01 00:00:00.000"},
        {"20210401000000+0830", "2021-03-31 23:30:00.000"},
        {"20210401000000Z", "2021-04-01 08:00:00.000"},
        {"20210401+08:00", "2021-04-01 00:00:00.000"},
        {"20210401+0830", "2021-03-31 23:30:00.000"},
        {"20210401Z", "2021-04-01 08:00:00.000"},
        {"20210401", "2021-04-01 00:00:00.000"},
    });

    /// Basic format: not match or partial match
    checkISOFormat(
        {{"20210401T0100.+08:00", "2021-04-01 01:00:00.000"},
         {"20210401T0100.000Z", "2021-04-01 01:00:00.000"},
         {"20210401T0100+08:00", "2021-04-01 01:00:00.000"},
         {"20210401T0100+0830", "2021-04-01 01:00:00.000"},
         {"20210401T0100Z", "2021-04-01 01:00:00.000"},
         {"20210401T0100", "2021-04-01 01:00:00.000"},
         {"20210401T01.001Z", "2021-04-01 01:00:00.000"},
         {"202104010000.+08:00", "2021-04-01 00:00:00.000"},
         {"202104010000.000Z", "2021-04-01 00:00:00.000"},
         {"2021040101.001Z", "2021-04-01 00:00:00.000"},
         {"2021040100Z", "2021-04-01 00:00:00.000"},
         {"202104010000Z", "2021-04-01 00:00:00.000"},
         {"202104", "1970-01-01 08:00:00.000"}});
}

TEST(RawStoreFormatTest, AutoExtractJSON)
{
    String str = R"###([
"_raw": [{"prop1": "2002-12-15 12:00:00.234+01:30"}, {"propb": 123}]
])###";


    /// Extension format
    checkISOFormat(
        {{"2002-12-15 12:00:00.234+01:30", "2002-12-15 18:30:00.234"},
         {"2002-12-15 12:00:00.234+01:30", "2002-12-15 18:30:00.234"},
         {"2002-12-15T12:00:00.234+01:30", "2002-12-15 18:30:00.234"},
         {"2002-12-15 12:00:00.234Z", "2002-12-15 20:00:00.234"},
         {"2002-12-15T12:00:00.234Z", "2002-12-15 20:00:00.234"},
         {"2002-12-15 12:00:00.234+0130", "2002-12-15 18:30:00.234"},
         {"2002-12-15 12:00:00.234-0130", "2002-12-15 21:30:00.234"},
         {"2011-03-17T01:00:00-04:00", "2011-03-17 13:00:00.000"},
         {"2011-03-17T01:00:00+0400", "2011-03-17 05:00:00.000"},
         {"2002-12-15 12:00:00Z", "2002-12-15 20:00:00.000"},
         {"2002-12-15T12:00:00+04", "2002-12-15 16:00:00.000"},
         {"2002-12-15T12:00+08:30", "2002-12-15 11:30:00.000"},
         {"2002-12-15T12:00+0430", "2002-12-15 15:30:00.000"},
         {"2022-12-15T12+04:30", "2022-12-15 15:30:00.000"},
         {"2022-12-15 12+04:30", "2022-12-15 15:30:00.000"},
         {"2022-12-15 12Z", "2022-12-15 20:00:00.000"},
         {"2022-12-15T12Z", "2022-12-15 20:00:00.000"},
         {"2002-12-15+04:30", "2002-12-15 03:30:00.000"},
         {"2002-12-15+0430", "2002-12-15 03:30:00.000"},
         {"2002-12-15Z", "2002-12-15 08:00:00.000"}});

    /// Extension format: not match or partial match
    checkISOFormat(
        {{"2002", "1970-01-01 08:00:00.000"},
         {"2002-12-15T12:00.123+0430", "2002-12-15 12:00:00.000"},
         {"2022-12-15T12.123+04:30", "2022-12-15 12:00:00.000"},
         {"2002-12-15+", "2002-12-15 00:00:00.000"}});

    /// Basic format
    checkISOFormat({
        {"20210401T000000.123+08:00", "2021-04-01 00:00:00.123"},
        {"20210401T000000.123+0830", "2021-03-31 23:30:00.123"},
        {"20210401T000000.123-08:00", "2021-04-01 16:00:00.123"},
        {"20210401T000000.123-08", "2021-04-01 16:00:00.123"},
        {"20210401T000000.123Z", "2021-04-01 08:00:00.123"},
        {"20210401T000000.123", "2021-04-01 00:00:00.123"},
        {"20210401T000000+08:00", "2021-04-01 00:00:00.000"},
        {"20210401T000000+0830", "2021-03-31 23:30:00.000"},
        {"20210401T000000-08:00", "2021-04-01 16:00:00.000"},
        {"20210401T000000-08", "2021-04-01 16:00:00.000"},
        {"20210401T000000Z", "2021-04-01 08:00:00.000"},
        {"20210401T00+08:00", "2021-04-01 00:00:00.000"},
        {"20210401T00+0830", "2021-03-31 23:30:00.000"},
        {"20210401T00Z", "2021-04-01 08:00:00.000"},
        {"20210401T00", "2021-04-01 00:00:00.000"},
        {"20210401000000.123+08:00", "2021-04-01 00:00:00.123"},
        {"20210401000000.123+0830", "2021-03-31 23:30:00.123"},
        {"20210401000000.123-08:00", "2021-04-01 16:00:00.123"},
        {"20210401000000.123-08", "2021-04-01 16:00:00.123"},
        {"20210401000000.123Z", "2021-04-01 08:00:00.123"},
        {"20210401000000.123", "2021-04-01 00:00:00.123"},
        {"20210401000000+08:00", "2021-04-01 00:00:00.000"},
        {"20210401000000+0830", "2021-03-31 23:30:00.000"},
        {"20210401000000Z", "2021-04-01 08:00:00.000"},
        {"20210401+08:00", "2021-04-01 00:00:00.000"},
        {"20210401+0830", "2021-03-31 23:30:00.000"},
        {"20210401Z", "2021-04-01 08:00:00.000"},
        {"20210401", "2021-04-01 00:00:00.000"},
    });

    /// Basic format: not match or partial match
    checkISOFormat(
        {{"20210401T0100.+08:00", "2021-04-01 01:00:00.000"},
         {"20210401T0100.000Z", "2021-04-01 01:00:00.000"},
         {"20210401T0100+08:00", "2021-04-01 01:00:00.000"},
         {"20210401T0100+0830", "2021-04-01 01:00:00.000"},
         {"20210401T0100Z", "2021-04-01 01:00:00.000"},
         {"20210401T0100", "2021-04-01 01:00:00.000"},
         {"20210401T01.001Z", "2021-04-01 01:00:00.000"},
         {"202104010000.+08:00", "2021-04-01 00:00:00.000"},
         {"202104010000.000Z", "2021-04-01 00:00:00.000"},
         {"2021040101.001Z", "2021-04-01 00:00:00.000"},
         {"2021040100Z", "2021-04-01 00:00:00.000"},
         {"202104010000Z", "2021-04-01 00:00:00.000"},
         {"202104", "1970-01-01 08:00:00.000"}});
}

TEST(RawStoreFormatTest, Exceptions)
{
    Block sample;
    {
        ColumnWithTypeAndName col;
        col.name = "a";
        col.type = std::make_shared<DataTypeUInt64>();
        sample.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_raw";
        col.type = std::make_shared<DataTypeString>();
        sample.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_time";
        col.type = std::make_shared<DataTypeDateTime64>(DataTypeDateTime64::default_scale, String{"Asia/Shanghai"});
        sample.insert(std::move(col));
    }

    String str = R"###({ "_raw": "2021-03-21 00:10:23, [Apache] This is a error."})###";

    String json = R"###([
                  "_raw": "{\"log\":{\"time\":\"2021-03-21 00:10:23\"}}"
                  ])###";

    FormatSettings format_settings;

    /// JSON: unable to extract _time
    format_settings.rawstore.rawstore_time_extraction_type = "json_path";
    format_settings.rawstore.rawstore_time_extraction_rule = "log.time1";
    checkException(sample, format_settings, json);

    /// Regex: unable to extract _time
    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(?P<_time>\w+),\s+\[\w+\])";
    checkException(sample, format_settings, str);

    /// Invalid time
    format_settings.rawstore.rawstore_time_extraction_type = "json_path";
    format_settings.rawstore.rawstore_time_extraction_rule = "log";
    checkException(sample, format_settings, json);

    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(.+),\s+\[(?P<_time>\w+)\])";
    checkException(sample, format_settings, str);
}

TEST(RawStoreFormatTest, ExceptionOfConstructor)
{
    Block sample;
    {
        ColumnWithTypeAndName col;
        col.name = "a";
        col.type = std::make_shared<DataTypeUInt64>();
        sample.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_raw";
        col.type = std::make_shared<DataTypeString>();
        sample.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_time";
        col.type = std::make_shared<DataTypeDateTime64>(DataTypeDateTime64::default_scale, String{"Asia/Shanghai"});
        sample.insert(std::move(col));
    }

    String str = R"###({ "_raw": "2021-03-21 00:10:23, [Apache] This is a error."})###";

    String json = R"###("[ "_raw": "{\"log\":{\"time\":\"2021-03-21 00:10:23\"}}"])###";

    FormatSettings format_settings;

    /// No rule of regex
    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    checkCtorException(sample, format_settings, str);

    /// No rule of json
    format_settings.rawstore.rawstore_time_extraction_type = "json_path";
    checkCtorException(sample, format_settings, json);

    /// Invalid rawstore_time_extraction_type
    format_settings.rawstore.rawstore_time_extraction_type = "other";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(?P_time1>.+),\s+\[\w+\])";
    checkCtorException(sample, format_settings, str);

    /// No _time group in regex
    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(?P<_time1>.+),\s+\[\w+\])";
    checkCtorException(sample, format_settings, str);

    /// Invalid regex
    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(?P.+),\s+\[\w+\])";
    checkCtorException(sample, format_settings, str);
}

TEST(RawStoreFormatTest, InvalidBlock)
{
    String str = R"###({ "_raw": "2021-03-21 00:10:23, [Apache] This is a error."})###";
    FormatSettings format_settings;

    /// No _raw column
    Block sample1;
    {
        ColumnWithTypeAndName col;
        col.name = "a";
        col.type = std::make_shared<DataTypeUInt64>();
        sample1.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "raw";
        col.type = std::make_shared<DataTypeString>();
        sample1.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_time";
        col.type = std::make_shared<DataTypeDateTime64>(DataTypeDateTime64::default_scale, String{"Asia/Shanghai"});
        sample1.insert(std::move(col));
    }


    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(?P<_time>.+),\s+\[\w+\])";
    checkCtorException(sample1, format_settings, str);

    format_settings.rawstore.rawstore_time_extraction_type = "json_path";
    format_settings.rawstore.rawstore_time_extraction_rule = "log.time";
    checkCtorException(sample1, format_settings, str);

    /// No _time column
    Block sample2;
    {
        ColumnWithTypeAndName col;
        col.name = "a";
        col.type = std::make_shared<DataTypeUInt64>();
        sample2.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_raw";
        col.type = std::make_shared<DataTypeString>();
        sample2.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "time";
        col.type = std::make_shared<DataTypeDateTime64>(DataTypeDateTime64::default_scale, String{"Asia/Shanghai"});
        sample2.insert(std::move(col));
    }
    format_settings.rawstore.rawstore_time_extraction_type = "regex";
    format_settings.rawstore.rawstore_time_extraction_rule = R"(^(?P<_time>.+),\s+\[\w+\])";
    checkCtorException(sample2, format_settings, str);

    format_settings.rawstore.rawstore_time_extraction_type = "json_path";
    format_settings.rawstore.rawstore_time_extraction_rule = "log.time";
    checkCtorException(sample2, format_settings, str);

    /// _raw column is not String, json and regex
    Block sample3;
    {
        ColumnWithTypeAndName col;
        col.name = "a";
        col.type = std::make_shared<DataTypeUInt64>();
        sample3.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_raw";
        col.type = std::make_shared<DataTypeUInt64>();
        sample3.insert(std::move(col));
    }
    {
        ColumnWithTypeAndName col;
        col.name = "_time";
        col.type = std::make_shared<DataTypeDateTime64>(DataTypeDateTime64::default_scale, String{"Asia/Shanghai"});
        sample3.insert(std::move(col));
    }
    String str1 = "{"
                  "\"_raw\": 65"
                  "}";

    format_settings.rawstore.rawstore_time_extraction_type = "json_path";
    format_settings.rawstore.rawstore_time_extraction_rule = "log.time";
    checkException(sample3, format_settings, str);
}
