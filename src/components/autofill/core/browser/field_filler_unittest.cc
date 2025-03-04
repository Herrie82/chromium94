// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill/core/browser/field_filler.h"

#include <stddef.h>

#include <memory>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/ranges/algorithm.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "components/autofill/core/browser/address_normalizer.h"
#include "components/autofill/core/browser/address_normalizer_impl.h"
#include "components/autofill/core/browser/autofill_field.h"
#include "components/autofill/core/browser/autofill_test_utils.h"
#include "components/autofill/core/browser/autofill_type.h"
#include "components/autofill/core/browser/data_model/autofill_profile.h"
#include "components/autofill/core/browser/data_model/credit_card.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/browser/geo/alternative_state_name_map_test_utils.h"
#include "components/autofill/core/browser/geo/country_names.h"
#include "components/autofill/core/common/autofill_features.h"
#include "components/autofill/core/common/autofill_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/libaddressinput/src/cpp/include/libaddressinput/null_storage.h"
#include "third_party/libaddressinput/src/cpp/include/libaddressinput/source.h"
#include "third_party/libaddressinput/src/cpp/include/libaddressinput/storage.h"
#include "third_party/libaddressinput/src/cpp/test/testdata_source.h"

using base::StringToInt;

namespace autofill {

using ::i18n::addressinput::NullStorage;
using ::i18n::addressinput::Source;
using ::i18n::addressinput::Storage;
using ::i18n::addressinput::TestdataSource;

const std::vector<const char*> NotNumericMonthsContentsNoPlaceholder() {
  const std::vector<const char*> result = {"Jan", "Feb", "Mar", "Apr",
                                           "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec"};
  return result;
}

const std::vector<const char*> NotNumericMonthsContentsWithPlaceholder() {
  const std::vector<const char*> result = {"Select a Month",
                                           "Jan",
                                           "Feb",
                                           "Mar",
                                           "Apr",
                                           "May",
                                           "Jun",
                                           "Jul",
                                           "Aug",
                                           "Sep",
                                           "Oct",
                                           "Nov",
                                           "Dec"};
  return result;
}

// Returns the index of |value| in |values|.
size_t GetIndexOfValue(const std::vector<SelectOption>& values,
                       const std::u16string& value) {
  size_t i =
      base::ranges::find(values, value, &SelectOption::value) - values.begin();
  CHECK_LT(i, values.size()) << "Passing invalid arguments to GetIndexOfValue";
  return i;
}

// Creates the select field from the specified |values| and |contents| and tests
// filling with 3 different values.
void TestFillingExpirationMonth(const std::vector<const char*>& values,
                                const std::vector<const char*>& contents,
                                size_t select_size) {
  // Create the select field.
  AutofillField field;
  test::CreateTestSelectField("", "", "", values, contents, select_size,
                              &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_MONTH);

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);

  size_t content_index = 0;
  // Try a single-digit month.
  CreditCard card = test::GetCreditCard();
  card.SetExpirationMonth(3);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  content_index = GetIndexOfValue(field.options, field.value);
  EXPECT_EQ(u"Mar", field.options[content_index].content);

  // Try a two-digit month.
  card.SetExpirationMonth(11);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  content_index = GetIndexOfValue(field.options, field.value);
  EXPECT_EQ(u"Nov", field.options[content_index].content);
}

void TestFillingInvalidFields(const std::u16string& state,
                              const std::u16string& city) {
  AutofillProfile profile = test::GetFullProfile();
  profile.SetValidityState(ADDRESS_HOME_STATE, AutofillProfile::INVALID,
                           AutofillProfile::SERVER);
  profile.SetValidityState(ADDRESS_HOME_CITY, AutofillProfile::INVALID,
                           AutofillProfile::CLIENT);

  AutofillField field_state;
  field_state.set_heuristic_type(ADDRESS_HOME_STATE);
  AutofillField field_city;
  field_city.set_heuristic_type(ADDRESS_HOME_CITY);

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field_state, &profile, &field_state,
                       /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(state, field_state.value);

  filler.FillFormField(field_city, &profile, &field_city,
                       /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(city, field_city.value);
}

struct CreditCardTestCase {
  std::u16string card_number_;
  size_t total_splits_;
  std::vector<int> splits_;
  std::vector<std::u16string> expected_results_;
};

// Returns the offset to be set within the credit card number field.
size_t GetNumberOffset(size_t index, const CreditCardTestCase& test) {
  size_t result = 0;
  for (size_t i = 0; i < index; ++i)
    result += test.splits_[i];
  return result;
}

class AutofillFieldFillerTest : public testing::Test {
 protected:
  AutofillFieldFillerTest()
      : credit_card_(test::GetCreditCard()), address_(test::GetFullProfile()) {
    CountryNames::SetLocaleString("en-US");
  }

  CreditCard* credit_card() { return &credit_card_; }
  AutofillProfile* address() { return &address_; }

 private:
  CreditCard credit_card_;
  AutofillProfile address_;

  DISALLOW_COPY_AND_ASSIGN(AutofillFieldFillerTest);
};

TEST_F(AutofillFieldFillerTest, Type) {
  AutofillField field;
  AutofillQueryResponse::FormSuggestion::FieldSuggestion::FieldPrediction
      prediction;

  ASSERT_EQ(NO_SERVER_DATA, field.server_type());
  ASSERT_EQ(UNKNOWN_TYPE, field.heuristic_type());

  // |server_type_| is NO_SERVER_DATA, so |heuristic_type_| is returned.
  EXPECT_EQ(UNKNOWN_TYPE, field.Type().GetStorableType());

  // Set the heuristic type and check it.
  field.set_heuristic_type(NAME_FIRST);
  EXPECT_EQ(NAME_FIRST, field.Type().GetStorableType());
  EXPECT_EQ(FieldTypeGroup::kName, field.Type().group());

  // Set the server type and check it.
  prediction.set_type(ADDRESS_BILLING_LINE1);
  field.set_server_predictions({prediction});
  EXPECT_EQ(ADDRESS_HOME_LINE1, field.Type().GetStorableType());
  EXPECT_EQ(FieldTypeGroup::kAddressBilling, field.Type().group());

  // Checks that overall_type trumps everything.
  field.SetTypeTo(AutofillType(ADDRESS_BILLING_ZIP));
  EXPECT_EQ(ADDRESS_HOME_ZIP, field.Type().GetStorableType());
  EXPECT_EQ(FieldTypeGroup::kAddressBilling, field.Type().group());

  // Checks that setting server type resets overall type.
  prediction.set_type(ADDRESS_BILLING_LINE1);
  field.set_server_predictions({prediction});
  EXPECT_EQ(ADDRESS_HOME_LINE1, field.Type().GetStorableType());
  EXPECT_EQ(FieldTypeGroup::kAddressBilling, field.Type().group());

  // Remove the server type to make sure the heuristic type is preserved.
  prediction.set_type(NO_SERVER_DATA);
  field.set_server_predictions({prediction});
  EXPECT_EQ(NAME_FIRST, field.Type().GetStorableType());
  EXPECT_EQ(FieldTypeGroup::kName, field.Type().group());

  // Checks that overall_type trumps everything.
  field.SetTypeTo(AutofillType(ADDRESS_BILLING_ZIP));
  EXPECT_EQ(ADDRESS_HOME_ZIP, field.Type().GetStorableType());
  EXPECT_EQ(FieldTypeGroup::kAddressBilling, field.Type().group());

  // Set the heuristic type and check it and reset overall Type.
  field.set_heuristic_type(NAME_FIRST);
  EXPECT_EQ(NAME_FIRST, field.Type().GetStorableType());
  EXPECT_EQ(FieldTypeGroup::kName, field.Type().group());
}

// Tests that a credit card related prediction made by the heuristics overrides
// an unrecognized autocomplete attribute.
TEST_F(AutofillFieldFillerTest, Type_CreditCardOverrideHtml_Heuristics) {
  AutofillField field;

  field.SetHtmlType(HTML_TYPE_UNRECOGNIZED, HTML_MODE_NONE);

  // A credit card heuristic prediction overrides the unrecognized type.
  field.set_heuristic_type(CREDIT_CARD_NUMBER);
  EXPECT_EQ(CREDIT_CARD_NUMBER, field.Type().GetStorableType());

  // A non credit card heuristic prediction doesn't override the unrecognized
  // type.
  field.set_heuristic_type(NAME_FIRST);
  EXPECT_EQ(UNKNOWN_TYPE, field.Type().GetStorableType());

  // A credit card heuristic prediction doesn't override a known specified html
  // type.
  field.SetHtmlType(HTML_TYPE_NAME, HTML_MODE_NONE);
  field.set_heuristic_type(CREDIT_CARD_NUMBER);
  EXPECT_EQ(NAME_FULL, field.Type().GetStorableType());
}

// Tests that a credit card related prediction made by the server overrides an
// unrecognized autocomplete attribute.
TEST_F(AutofillFieldFillerTest, Type_CreditCardOverrideHtml_ServerPredicitons) {
  AutofillField field;
  AutofillQueryResponse::FormSuggestion::FieldSuggestion::FieldPrediction
      prediction;

  field.SetHtmlType(HTML_TYPE_UNRECOGNIZED, HTML_MODE_NONE);

  // A credit card server prediction overrides the unrecognized type.
  prediction.set_type(CREDIT_CARD_NUMBER);
  field.set_server_predictions({prediction});
  EXPECT_EQ(CREDIT_CARD_NUMBER, field.Type().GetStorableType());

  // A non credit card server prediction doesn't override the unrecognized
  // type.
  prediction.set_type(NAME_FIRST);
  field.set_server_predictions({prediction});
  EXPECT_EQ(UNKNOWN_TYPE, field.Type().GetStorableType());

  // A credit card server prediction doesn't override a known specified html
  // type.
  field.SetHtmlType(HTML_TYPE_NAME, HTML_MODE_NONE);
  prediction.set_type(CREDIT_CARD_NUMBER);
  field.set_server_predictions({prediction});
  EXPECT_EQ(NAME_FULL, field.Type().GetStorableType());
}

// Tests that if both autocomplete attibutes and server agree it's a phone
// field, always use server predicted type. If they disagree with autocomplete
// says it's a phone field, always use autocomplete attribute.
TEST_F(AutofillFieldFillerTest,
       Type_ServerPredictionOfCityAndNumber_OverrideHtml) {
  AutofillField field;
  AutofillQueryResponse::FormSuggestion::FieldSuggestion::FieldPrediction
      prediction;

  field.SetHtmlType(HTML_TYPE_TEL, HTML_MODE_NONE);

  prediction.set_type(PHONE_HOME_CITY_AND_NUMBER);
  field.set_server_predictions({prediction});
  EXPECT_EQ(PHONE_HOME_CITY_AND_NUMBER, field.Type().GetStorableType());

  // Overrides to another number format.
  prediction.set_type(PHONE_HOME_NUMBER);
  field.set_server_predictions({prediction});
  EXPECT_EQ(PHONE_HOME_NUMBER, field.Type().GetStorableType());

  // Overrides autocomplete=tel-national too.
  field.SetHtmlType(HTML_TYPE_TEL_NATIONAL, HTML_MODE_NONE);
  prediction.set_type(PHONE_HOME_WHOLE_NUMBER);
  field.set_server_predictions({prediction});
  EXPECT_EQ(PHONE_HOME_WHOLE_NUMBER, field.Type().GetStorableType());

  // If autocomplete=tel-national but server says it's not a phone field,
  // do not override.
  field.SetHtmlType(HTML_TYPE_TEL_NATIONAL, HTML_MODE_NONE);
  prediction.set_type(CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR);
  field.set_server_predictions({prediction});
  EXPECT_EQ(PHONE_HOME_CITY_AND_NUMBER, field.Type().GetStorableType());

  // If html type not specified, we still use server prediction.
  field.SetHtmlType(HTML_TYPE_UNSPECIFIED, HTML_MODE_NONE);
  prediction.set_type(PHONE_HOME_CITY_AND_NUMBER);
  field.set_server_predictions({prediction});
  EXPECT_EQ(PHONE_HOME_CITY_AND_NUMBER, field.Type().GetStorableType());
}

TEST_F(AutofillFieldFillerTest, IsEmpty) {
  AutofillField field;
  ASSERT_EQ(std::u16string(), field.value);

  // Field value is empty.
  EXPECT_TRUE(field.IsEmpty());

  // Field value is non-empty.
  field.value = u"Value";
  EXPECT_FALSE(field.IsEmpty());
}

TEST_F(AutofillFieldFillerTest, FieldSignatureAsStr) {
  AutofillField field;
  AutofillQueryResponse::FormSuggestion::FieldSuggestion::FieldPrediction
      prediction;
  ASSERT_EQ(std::u16string(), field.name);
  ASSERT_EQ(std::string(), field.form_control_type);

  // Signature is empty.
  EXPECT_EQ("2085434232", field.FieldSignatureAsStr());

  // Field name is set.
  field.name = u"Name";
  EXPECT_EQ("1606968241", field.FieldSignatureAsStr());

  // Field form control type is set.
  field.form_control_type = "text";
  EXPECT_EQ("502192749", field.FieldSignatureAsStr());

  // Heuristic type does not affect FieldSignature.
  field.set_heuristic_type(NAME_FIRST);
  EXPECT_EQ("502192749", field.FieldSignatureAsStr());

  // Server type does not affect FieldSignature.
  prediction.set_type(NAME_LAST);
  field.set_server_predictions({prediction});
  EXPECT_EQ("502192749", field.FieldSignatureAsStr());
}

TEST_F(AutofillFieldFillerTest, IsFieldFillable) {
  AutofillField field;
  AutofillQueryResponse::FormSuggestion::FieldSuggestion::FieldPrediction
      prediction;
  ASSERT_EQ(UNKNOWN_TYPE, field.Type().GetStorableType());

  // Type is unknown.
  EXPECT_FALSE(field.IsFieldFillable());

  // Only heuristic type is set.
  field.set_heuristic_type(NAME_FIRST);
  EXPECT_TRUE(field.IsFieldFillable());

  // Only server type is set.
  field.set_heuristic_type(UNKNOWN_TYPE);
  prediction.set_type(NAME_LAST);
  field.set_server_predictions({prediction});
  EXPECT_TRUE(field.IsFieldFillable());

  // Both types set.
  field.set_heuristic_type(NAME_FIRST);
  prediction.set_type(NAME_LAST);
  field.set_server_predictions({prediction});
  EXPECT_TRUE(field.IsFieldFillable());

  // Field has autocomplete="off" set. Since autofill was able to make a
  // prediction, it is still considered a fillable field.
  field.should_autocomplete = false;
  EXPECT_TRUE(field.IsFieldFillable());
}

// Verify that non credit card related fields with the autocomplete attribute
// set to off are filled on desktop when the feature to Autofill all
// addresses is enabled (default).
TEST_F(AutofillFieldFillerTest,
       FillFormField_AutocompleteOffNotRespected_AddressField) {
  AutofillField field;
  field.should_autocomplete = false;
  field.set_heuristic_type(NAME_FIRST);

  // Non credit card related field.
  address()->SetRawInfo(NAME_FIRST, u"Test");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, address(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify that the field is filled in all circumstances.
  EXPECT_EQ(u"Test", field.value);
}

// Verify that credit card related fields with the autocomplete attribute
// set to off get filled.
TEST_F(AutofillFieldFillerTest, FillFormField_AutocompleteOff_CreditCardField) {
  AutofillField field;
  field.should_autocomplete = false;
  field.set_heuristic_type(CREDIT_CARD_NUMBER);

  // Credit card related field.
  credit_card()->SetNumber(u"4111111111111111");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, credit_card(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify that the field is filled.
  EXPECT_EQ(u"4111111111111111", field.value);
}

// Verify that the correct value is returned if the maximum length of the credit
// card value exceeds the actual length.
TEST_F(AutofillFieldFillerTest,
       FillFormField_MaxLength_CreditCardField_MaxLenghtExceedsLength) {
  AutofillField field;
  field.max_length = 30;
  field.set_credit_card_number_offset(2);
  field.set_heuristic_type(CREDIT_CARD_NUMBER);

  // Credit card related field.
  credit_card()->SetNumber(u"0123456789999999");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, credit_card(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify that the field is filled with the fourth digit of the credit card
  // number.
  EXPECT_EQ(u"23456789999999", field.value);
}

// Verify that the full credit card number is returned if the offset exceeds the
// length.
TEST_F(AutofillFieldFillerTest,
       FillFormField_MaxLength_CreditCardField_OffsetExceedsLength) {
  AutofillField field;
  field.max_length = 18;
  field.set_credit_card_number_offset(30);
  field.set_heuristic_type(CREDIT_CARD_NUMBER);

  // Credit card related field.
  credit_card()->SetNumber(u"0123456789999999");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, credit_card(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify that the field is filled with the full credit card number.
  EXPECT_EQ(u"0123456789999999", field.value);
}

// Verify that only the truncated and offsetted value of the credit card number
// is set.
TEST_F(AutofillFieldFillerTest,
       FillFormField_MaxLength_CreditCardField_WithOffset) {
  AutofillField field;
  field.max_length = 1;
  field.set_credit_card_number_offset(3);
  field.set_heuristic_type(CREDIT_CARD_NUMBER);

  // Credit card related field.
  credit_card()->SetNumber(u"0123456789999999");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, credit_card(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify that the field is filled with the third digit of the credit card
  // number.
  EXPECT_EQ(u"3", field.value);
}

// Verify that only the truncated value of the credit card number is set.
TEST_F(AutofillFieldFillerTest, FillFormField_MaxLength_CreditCardField) {
  AutofillField field;
  field.max_length = 1;
  field.set_heuristic_type(CREDIT_CARD_NUMBER);

  // Credit card related field.
  credit_card()->SetNumber(u"4111111111111111");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, credit_card(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify that the field is filled with only the first digit of the credit
  // card number.
  EXPECT_EQ(u"4", field.value);
}

// Test that in the preview credit card numbers are obfuscated.
TEST_F(AutofillFieldFillerTest, FillFormField_Preview_CreditCardField) {
  AutofillField field;
  field.set_heuristic_type(CREDIT_CARD_NUMBER);

  // Credit card related field.
  credit_card()->SetNumber(u"4111111111111111");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, credit_card(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kPreview);

  // Verify that the field contains 4 but no more than 4 digits.
  size_t num_digits =
      base::ranges::count_if(field.value, &base::IsAsciiDigit<char16_t>);
  EXPECT_EQ(4u, num_digits);
}

// Verify that when the relevant feature is enabled, the invalid fields don't
// get filled.
TEST_F(AutofillFieldFillerTest, FillFormField_Validity_ServerClient) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{features::kAutofillProfileServerValidation,
                            features::kAutofillProfileClientValidation},
      /*disabled_features=*/{});
  // State's validity is set by server and city's validity by client.
  TestFillingInvalidFields(/*state=*/std::u16string(),
                           /*city=*/std::u16string());
}

TEST_F(AutofillFieldFillerTest, FillFormField_Validity_OnlyServer) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{features::kAutofillProfileServerValidation},
      /*disabled_features=*/{features::kAutofillProfileClientValidation});
  // State's validity is set by server and city's validity by client.
  TestFillingInvalidFields(/*state=*/std::u16string(),
                           /*city=*/u"Elysium");
}

TEST_F(AutofillFieldFillerTest, FillFormField_Validity_OnlyClient) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{features::kAutofillProfileClientValidation},
      /*disabled_features=*/{features::kAutofillProfileServerValidation});
  // State's validity is set by server and city's validity by client.
  TestFillingInvalidFields(/*state=*/u"CA",
                           /*city=*/std::u16string());
}

TEST_F(AutofillFieldFillerTest, FillFormField_NoValidity) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{},
      /*disabled_features=*/{features::kAutofillProfileServerValidation,
                             features::kAutofillProfileClientValidation});
  // State's validity is set by server and city's validity by client.
  TestFillingInvalidFields(/*state=*/u"CA",
                           /*city=*/u"Elysium");
}

// Tests that using only client side validation, if the country is empty, the
// address fields will get filled regardless of their invalidity.
TEST_F(AutofillFieldFillerTest, FillFormField_Validity_CountryEmpty) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{features::kAutofillProfileClientValidation},
      /*disabled_features=*/{features::kAutofillProfileServerValidation});
  AutofillProfile profile = test::GetFullProfile();
  profile.SetRawInfo(ADDRESS_HOME_COUNTRY, u"");
  profile.SetValidityState(ADDRESS_HOME_STATE, AutofillProfile::INVALID,
                           AutofillProfile::CLIENT);
  profile.SetValidityState(EMAIL_ADDRESS, AutofillProfile::INVALID,
                           AutofillProfile::CLIENT);

  AutofillField field_state;
  field_state.set_heuristic_type(ADDRESS_HOME_STATE);
  AutofillField field_email;
  field_email.set_heuristic_type(EMAIL_ADDRESS);

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  // State is filled, because it's an address field.
  filler.FillFormField(field_state, &profile, &field_state,
                       /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"CA", field_state.value);

  // Email is not filled, because it's not an address field, and it doesn't
  // depend on the country.
  filler.FillFormField(field_email, &profile, &field_email,
                       /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"", field_email.value);
}

struct AutofillFieldFillerTestCase {
  HtmlFieldType field_type;
  size_t field_max_length;
  std::u16string expected_value;

  AutofillFieldFillerTestCase(HtmlFieldType field_type,
                              size_t field_max_length,
                              std::u16string expected_value)
      : field_type(field_type),
        field_max_length(field_max_length),
        expected_value(expected_value) {}
};

struct AutofillPhoneFieldFillerTestCase : public AutofillFieldFillerTestCase {
  std::u16string phone_home_whole_number_value;

  AutofillPhoneFieldFillerTestCase(HtmlFieldType field_type,
                                   size_t field_max_length,
                                   std::u16string expected_value,
                                   std::u16string phone_home_whole_number_value)
      : AutofillFieldFillerTestCase(field_type,
                                    field_max_length,
                                    expected_value),
        phone_home_whole_number_value(phone_home_whole_number_value) {}
};

class PhoneNumberTest
    : public testing::TestWithParam<AutofillPhoneFieldFillerTestCase> {
 public:
  PhoneNumberTest() { CountryNames::SetLocaleString("en-US"); }
};

TEST_P(PhoneNumberTest, FillPhoneNumber) {
  auto test_case = GetParam();
  AutofillField field;
  field.SetHtmlType(test_case.field_type, HtmlFieldMode());
  field.max_length = test_case.field_max_length;

  AutofillProfile address;
  address.SetRawInfo(PHONE_HOME_WHOLE_NUMBER,
                     test_case.phone_home_whole_number_value);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(test_case.expected_value, field.value);
}

INSTANTIATE_TEST_SUITE_P(
    AutofillFieldFillerTest,
    PhoneNumberTest,
    testing::Values(
        // Filling a prefix type field should just fill the prefix.
        AutofillPhoneFieldFillerTestCase{HTML_TYPE_TEL_LOCAL_PREFIX,
                                         /*field_max_length=*/0, u"555",
                                         u"+15145554578"},
        // Filling a suffix type field with a phone number of 7 digits should
        // just fill the suffix.
        AutofillPhoneFieldFillerTestCase{HTML_TYPE_TEL_LOCAL_SUFFIX,
                                         /*field_max_length=*/0, u"4578",
                                         u"+15145554578"},
        // Filling a phone type field with a max length of 3 should fill only
        // the prefix.
        AutofillPhoneFieldFillerTestCase{HTML_TYPE_TEL_LOCAL,
                                         /*field_max_length=*/3, u"555",
                                         u"+15145554578"},
        // TODO(crbug.com/581485): There should be a test case where the full
        // number is requested (HTML_TYPE_TEL) but a field_max_length of 3 would
        // fill the prefix.
        // Filling a phone type field with a max length of 4 should fill only
        // the suffix.
        AutofillPhoneFieldFillerTestCase{HTML_TYPE_TEL,
                                         /*field_max_length=*/4, u"4578",
                                         u"+15145554578"},
        // Filling a phone type field with a max length of 10 with a phone
        // number including the country code should fill the phone number
        // without the country code.
        AutofillPhoneFieldFillerTestCase{HTML_TYPE_TEL,
                                         /*field_max_length=*/10, u"5145554578",
                                         u"+15145554578"},
        // Filling a phone type field with a max length of 5 with a phone number
        // should fill with the last 5 digits of that phone number.
        AutofillPhoneFieldFillerTestCase{HTML_TYPE_TEL,
                                         /*field_max_length=*/5, u"54578",
                                         u"+15145554578"},
        // Filling a phone type field with a max length of 10 with a phone
        // number including the country code should fill the phone number
        // without the country code.
        AutofillPhoneFieldFillerTestCase{HTML_TYPE_TEL,
                                         /*field_max_length=*/10, u"123456789",
                                         u"+886123456789"}));

class ExpirationYearTest
    : public testing::TestWithParam<AutofillFieldFillerTestCase> {
 public:
  ExpirationYearTest() { CountryNames::SetLocaleString("en-US"); }
};

TEST_P(ExpirationYearTest, FillExpirationYearInput) {
  auto test_case = GetParam();
  AutofillField field;
  field.form_control_type = "text";
  field.SetHtmlType(test_case.field_type, HtmlFieldMode());
  field.max_length = test_case.field_max_length;

  CreditCard card = test::GetCreditCard();
  card.SetExpirationDateFromString(u"12/2023");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(test_case.expected_value, field.value);
}

INSTANTIATE_TEST_SUITE_P(
    AutofillFieldFillerTest,
    ExpirationYearTest,
    testing::Values(
        // A field predicted as a 2 digits expiration year should fill the last
        // 2 digits of the expiration year if the field has an unspecified max
        // length (0) or if it's greater than 1.
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_2_DIGIT_YEAR,
                                    /* default value */ 0, u"23"},
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_2_DIGIT_YEAR, 2,
                                    u"23"},
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_2_DIGIT_YEAR, 12,
                                    u"23"},
        // A field predicted as a 2 digit expiration year should fill the last
        // digit of the expiration year if the field has a max length of 1.
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_2_DIGIT_YEAR, 1,
                                    u"3"},
        // A field predicted as a 4 digit expiration year should fill the 4
        // digits of the expiration year if the field has an unspecified max
        // length (0) or if it's greater than 3 .
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_4_DIGIT_YEAR,
                                    /* default value */ 0, u"2023"},
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_4_DIGIT_YEAR, 4,
                                    u"2023"},
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_4_DIGIT_YEAR, 12,
                                    u"2023"},
        // A field predicted as a 4 digits expiration year should fill the last
        // 2 digits of the expiration year if the field has a max length of 2.
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_4_DIGIT_YEAR, 2,
                                    u"23"},
        // A field predicted as a 4 digits expiration year should fill the last
        // digit of the expiration year if the field has a max length of 1.
        AutofillFieldFillerTestCase{HTML_TYPE_CREDIT_CARD_EXP_4_DIGIT_YEAR, 1,
                                    u"3"}));

struct FillUtilExpirationDateTestCase {
  HtmlFieldType field_type;
  size_t field_max_length;
  std::u16string expected_value;
  bool expected_response;
};

class ExpirationDateTest
    : public testing::TestWithParam<FillUtilExpirationDateTestCase> {
 public:
  ExpirationDateTest() { CountryNames::SetLocaleString("en-US"); }
};

TEST_P(ExpirationDateTest, FillExpirationDateInput) {
  auto test_case = GetParam();
  AutofillField field;
  field.form_control_type = "text";
  field.SetHtmlType(test_case.field_type, HtmlFieldMode());
  field.max_length = test_case.field_max_length;

  CreditCard card = test::GetCreditCard();
  card.SetExpirationDateFromString(u"03/2022");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  bool response = filler.FillFormField(field, &card, &field,
                                       /*cvc=*/std::u16string(),
                                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(test_case.expected_value, field.value);
  EXPECT_EQ(response, test_case.expected_response);
}

INSTANTIATE_TEST_SUITE_P(
    AutofillFieldFillerTest,
    ExpirationDateTest,
    testing::Values(
        // A field predicted as a expiration date w/ 2 digit year should fill
        // with a format of MM/YY unless it has max-length of:
        // 4: Use format MMYY
        // 6: Use format MMYYYY
        // 7: Use format MM/YYYY
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR,
            /* default value */ 0, u"03/22", true},
        // Unsupported max lengths of 1-3, fail
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 1, u"", false},
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 2, u"", false},
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 3, u"", false},
        // A max length of 4 indicates a format of MMYY.
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 4, u"0322", true},
        // A max length of 6 indicates a format of MMYYYY, the 21st century is
        // assumed.
        // Desired case of proper max length >= 5
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 5, u"03/22", true},
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 6, u"032022", true},
        // A max length of 7 indicates a format of MM/YYYY, the 21st century is
        // assumed.
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 7, u"03/2022", true},
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_2_DIGIT_YEAR, 12, u"03/22", true},

        // A field predicted as a expiration date w/ 4 digit year should fill
        // with a format of MM/YYYY unless it has max-length of:
        // 4: Use format MMYY
        // 5: Use format MM/YY
        // 6: Use format MMYYYY
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR,
            /* default value */ 0, u"03/2022", true},
        // Unsupported max lengths of 1-3, fail
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 1, u"", false},
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 2, u"", false},
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 3, u"", false},
        // A max length of 4 indicates a format of MMYY.
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 4, u"0322", true},
        // A max length of 5 indicates a format of MM/YY.
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 5, u"03/22", true},
        // A max length of 6 indicates a format of MMYYYY.
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 6, u"032022", true},
        // Desired case of proper max length >= 7
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 7, u"03/2022", true},
        FillUtilExpirationDateTestCase{
            HTML_TYPE_CREDIT_CARD_EXP_DATE_4_DIGIT_YEAR, 12, u"03/2022",
            true}));

TEST_F(AutofillFieldFillerTest, FillSelectControlByValue) {
  std::vector<const char*> kOptions = {
      "Eenie",
      "Meenie",
      "Miney",
      "Mo",
  };

  AutofillField field;
  test::CreateTestSelectField(kOptions, &field);
  field.set_heuristic_type(NAME_FIRST);

  // Set semantically empty contents for each option, so that only the values
  // can be used for matching.
  for (size_t i = 0; i < field.options.size(); ++i)
    field.options[i].content = base::NumberToString16(i);

  address()->SetRawInfo(NAME_FIRST, u"Meenie");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, address(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Meenie", field.value);
}

TEST_F(AutofillFieldFillerTest, FillSelectControlByContents) {
  std::vector<const char*> kOptions = {
      "Eenie",
      "Meenie",
      "Miney",
      "Mo",
  };
  AutofillField field;
  test::CreateTestSelectField(kOptions, &field);
  field.set_heuristic_type(NAME_FIRST);

  // Set semantically empty values for each option, so that only the contents
  // can be used for matching.
  for (size_t i = 0; i < field.options.size(); ++i)
    field.options[i].value = base::NumberToString16(i);

  address()->SetRawInfo(NAME_FIRST, u"Miney");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, address(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"2", field.value);  // Corresponds to "Miney".
}

struct FillSelectTestCase {
  std::vector<const char*> select_values;
  const char16_t* input_value;
  const char16_t* expected_value_without_normalization;
  const char16_t* expected_value_with_normalization = nullptr;
};

class AutofillSelectWithStatesTest
    : public testing::TestWithParam<FillSelectTestCase> {
 public:
  AutofillSelectWithStatesTest() {
    CountryNames::SetLocaleString("en-US");

    base::FilePath file_path;
    CHECK(base::PathService::Get(base::DIR_SOURCE_ROOT, &file_path));
    file_path = file_path.Append(FILE_PATH_LITERAL("third_party"))
                    .Append(FILE_PATH_LITERAL("libaddressinput"))
                    .Append(FILE_PATH_LITERAL("src"))
                    .Append(FILE_PATH_LITERAL("testdata"))
                    .Append(FILE_PATH_LITERAL("countryinfo.txt"));

    normalizer_ = std::make_unique<AddressNormalizerImpl>(
        std::unique_ptr<Source>(
            new TestdataSource(true, file_path.AsUTF8Unsafe())),
        std::unique_ptr<Storage>(new NullStorage), "en-US");

    test::PopulateAlternativeStateNameMapForTesting(
        "US", "California",
        {{.canonical_name = "California",
          .abbreviations = {"CA"},
          .alternative_names = {}}});
    test::PopulateAlternativeStateNameMapForTesting(
        "US", "North Carolina",
        {{.canonical_name = "North Carolina",
          .abbreviations = {"NC"},
          .alternative_names = {}}});
    // Make sure the normalizer is done initializing its member(s) in
    // background task(s).
    task_environment_.RunUntilIdle();
  }

 protected:
  AddressNormalizer* normalizer() { return normalizer_.get(); }

 private:
  base::test::TaskEnvironment task_environment_;
  std::unique_ptr<AddressNormalizerImpl> normalizer_;

  DISALLOW_COPY_AND_ASSIGN(AutofillSelectWithStatesTest);
};

TEST_P(AutofillSelectWithStatesTest, FillSelectWithStates) {
  auto test_case = GetParam();
  AutofillField field;
  test::CreateTestSelectField(test_case.select_values, &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);

  // Without a normalizer.
  AutofillProfile address = test::GetFullProfile();
  address.SetRawInfo(ADDRESS_HOME_STATE, test_case.input_value);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  // nullptr means we expect them not to match without normalization.
  if (test_case.expected_value_without_normalization != nullptr) {
    EXPECT_EQ(test_case.expected_value_without_normalization, field.value);
  }

  // With a normalizer.
  AutofillProfile canadian_address = test::GetFullCanadianProfile();
  canadian_address.SetRawInfo(ADDRESS_HOME_STATE, test_case.input_value);
  // Fill a first time without loading the rules for the region.
  FieldFiller canadian_filler(/*app_locale=*/"en-US", normalizer());
  canadian_filler.FillFormField(field, &canadian_address, &field,
                                /*cvc=*/std::u16string(),
                                mojom::RendererFormDataAction::kFill);
  // If the expectation with normalization is nullptr, this means that the same
  // result than without a normalizer is expected.
  if (test_case.expected_value_with_normalization == nullptr) {
    EXPECT_EQ(test_case.expected_value_without_normalization, field.value);
  } else {
    // We needed a normalizer with loaded rules. The first fill should have
    // failed.
    EXPECT_NE(test_case.expected_value_with_normalization, field.value);

    // Load the rules and try again.
    normalizer()->LoadRulesForRegion("CA");
    canadian_filler.FillFormField(field, &canadian_address, &field,
                                  /*cvc=*/std::u16string(),
                                  mojom::RendererFormDataAction::kFill);
    EXPECT_EQ(test_case.expected_value_with_normalization, field.value);
  }
}

INSTANTIATE_TEST_SUITE_P(
    AutofillFieldFillerTest,
    AutofillSelectWithStatesTest,
    testing::Values(
        // Filling the abbreviation.
        FillSelectTestCase{{"Alabama", "California"}, u"CA", u"California"},
        // Attempting to fill the full name in a select full of abbreviations.
        FillSelectTestCase{{"AL", "CA"}, u"California", u"CA"},
        // Different case and diacritics.
        FillSelectTestCase{{"QUÉBEC", "ALBERTA"}, u"Quebec", u"QUÉBEC"},
        // The value and the field options are different but normalize to the
        // same (NB).
        FillSelectTestCase{{"Nouveau-Brunswick", "Alberta"},
                           u"New Brunswick",
                           nullptr,
                           u"Nouveau-Brunswick"},
        FillSelectTestCase{{"NB", "AB"}, u"New Brunswick", nullptr, u"NB"},
        FillSelectTestCase{{"NB", "AB"}, u"Nouveau-Brunswick", nullptr, u"NB"},
        FillSelectTestCase{{"Nouveau-Brunswick", "Alberta"},
                           u"NB",
                           nullptr,
                           u"Nouveau-Brunswick"},
        FillSelectTestCase{{"New Brunswick", "Alberta"},
                           u"NB",
                           nullptr,
                           u"New Brunswick"},
        // Inexact state names.
        FillSelectTestCase{
            {"SC - South Carolina", "CA - California", "NC - North Carolina"},
            u"California",
            u"CA - California"},
        // Don't accidentally match "Virginia" to "West Virginia".
        FillSelectTestCase{
            {"WV - West Virginia", "VA - Virginia", "NV - North Virginia"},
            u"Virginia",
            u"VA - Virginia"},
        // Do accidentally match "Virginia" to "West Virginia".
        // TODO(crbug.com/624770): This test should not pass, but it does
        // because "Virginia" is a substring of "West Virginia".
        FillSelectTestCase{{"WV - West Virginia", "TX - Texas"},
                           u"Virginia",
                           u"WV - West Virginia"},
        // Tests that substring matches work for full state names (a full token
        // match isn't required). Also tests that matches work for states with
        // whitespace in the middle.
        FillSelectTestCase{{"California.", "North Carolina."},
                           u"North Carolina",
                           u"North Carolina."},
        FillSelectTestCase{{"NC - North Carolina", "CA - California"},
                           u"CA",
                           u"CA - California"},
        // These are not states.
        FillSelectTestCase{{"NCNCA", "SCNCA"}, u"NC", u""}));

TEST_F(AutofillFieldFillerTest, FillSelectWithCountries) {
  AutofillField field;
  test::CreateTestSelectField({"Albania", "Canada"}, &field);
  field.set_heuristic_type(ADDRESS_HOME_COUNTRY);

  AutofillProfile address = test::GetFullProfile();
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"CA");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Canada", field.value);
}

struct FillWithExpirationMonthTestCase {
  std::vector<const char*> select_values;
  std::vector<const char*> select_contents;
};

class AutofillSelectWithExpirationMonthTest
    : public testing::TestWithParam<FillWithExpirationMonthTestCase> {
 public:
  AutofillSelectWithExpirationMonthTest() {
    CountryNames::SetLocaleString("en-US");
  }
};

TEST_P(AutofillSelectWithExpirationMonthTest,
       FillSelectControlWithExpirationMonth) {
  auto test_case = GetParam();
  ASSERT_EQ(test_case.select_values.size(), test_case.select_contents.size());

  TestFillingExpirationMonth(test_case.select_values, test_case.select_contents,
                             test_case.select_values.size());
}

INSTANTIATE_TEST_SUITE_P(
    AutofillFieldFillerTest,
    AutofillSelectWithExpirationMonthTest,
    testing::Values(
        // Values start at 1.
        FillWithExpirationMonthTestCase{
            {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"},
            NotNumericMonthsContentsNoPlaceholder()},
        // Values start at 1 but single digits are whitespace padded!
        FillWithExpirationMonthTestCase{
            {" 1", " 2", " 3", " 4", " 5", " 6", " 7", " 8", " 9", "10", "11",
             "12"},
            NotNumericMonthsContentsNoPlaceholder()},
        // Values start at 0.
        FillWithExpirationMonthTestCase{
            {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"},
            NotNumericMonthsContentsNoPlaceholder()},
        // Values start at 00.
        FillWithExpirationMonthTestCase{
            {"00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "10",
             "11"},
            NotNumericMonthsContentsNoPlaceholder()},
        // The AngularJS framework can add a "number:" prefix to select values.
        FillWithExpirationMonthTestCase{
            {"number:1", "number:2", "number:3", "number:4", "number:5",
             "number:6", "number:7", "number:8", "number:9", "number:10",
             "number:11", "number:12"},
            NotNumericMonthsContentsNoPlaceholder()},
        // The AngularJS framework can add a "string:" prefix to select values.
        FillWithExpirationMonthTestCase{
            {"string:1", "string:2", "string:3", "string:4", "string:5",
             "string:6", "string:7", "string:8", "string:9", "string:10",
             "string:11", "string:12"},
            NotNumericMonthsContentsNoPlaceholder()},
        // Unexpected values that can be matched with the content.
        FillWithExpirationMonthTestCase{
            {"object:1", "object:2", "object:3", "object:4", "object:5",
             "object:6", "object:7", "object:8", "object:9", "object:10",
             "object:11", "object:12"},
            NotNumericMonthsContentsNoPlaceholder()},
        // Another example where unexpected values can be matched with the
        // content.
        FillWithExpirationMonthTestCase{
            {"object:a", "object:b", "object:c", "object:d", "object:e",
             "object:f", "object:g", "object:h", "object:i", "object:j",
             "object:k", "object:l"},
            NotNumericMonthsContentsNoPlaceholder()},
        // Another example where unexpected values can be matched with the
        // content.
        FillWithExpirationMonthTestCase{
            {"Farvardin", "Ordibehesht", "Khordad", "Tir", "Mordad",
             "Shahrivar", "Mehr", "Aban", "Azar", "Dey", "Bahman", "Esfand"},
            NotNumericMonthsContentsNoPlaceholder()},
        // Values start at 0 and the first content is a placeholder.
        FillWithExpirationMonthTestCase{
            {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11",
             "12"},
            NotNumericMonthsContentsWithPlaceholder()},
        // Values start at 1 and the first content is a placeholder.
        FillWithExpirationMonthTestCase{
            {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
             "13"},
            NotNumericMonthsContentsWithPlaceholder()},
        // Values start at 01 and the first content is a placeholder.
        FillWithExpirationMonthTestCase{
            {"01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11",
             "12", "13"},
            NotNumericMonthsContentsWithPlaceholder()},
        // Values start at 0 after a placeholder.
        FillWithExpirationMonthTestCase{
            {"?", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"},
            NotNumericMonthsContentsWithPlaceholder()},
        // Values start at 1 after a placeholder.
        FillWithExpirationMonthTestCase{
            {"?", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11",
             "12"},
            NotNumericMonthsContentsWithPlaceholder()},
        // Values start at 0 after a negative number.
        FillWithExpirationMonthTestCase{
            {"-1", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
             "11"},
            NotNumericMonthsContentsWithPlaceholder()},
        // Values start at 1 after a negative number.
        FillWithExpirationMonthTestCase{
            {"-1", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11",
             "12"},
            NotNumericMonthsContentsWithPlaceholder()}));

TEST_F(AutofillFieldFillerTest, FillSelectControlWithAbbreviatedMonthName) {
  std::vector<const char*> kMonthsAbbreviated = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  AutofillField field;
  test::CreateTestSelectField(kMonthsAbbreviated, &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_MONTH);

  CreditCard card = test::GetCreditCard();
  card.SetExpirationMonth(4);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Apr", field.value);
}

TEST_F(AutofillFieldFillerTest, FillSelectControlWithMonthName) {
  std::vector<const char*> kMonthsFull = {
      "January", "February", "March",     "April",   "May",      "June",
      "July",    "August",   "September", "October", "November", "December",
  };
  AutofillField field;
  test::CreateTestSelectField(kMonthsFull, &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_MONTH);

  CreditCard card = test::GetCreditCard();
  card.SetExpirationMonth(4);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"April", field.value);
}

TEST_F(AutofillFieldFillerTest, FillSelectControlWithMonthNameAndDigits) {
  std::vector<const char*> kMonthsFullWithDigits = {
      "January (01)",   "February (02)", "March (03)",    "April (04)",
      "May (05)",       "June (06)",     "July (07)",     "August (08)",
      "September (09)", "October (10)",  "November (11)", "December (12)",
  };
  AutofillField field;
  test::CreateTestSelectField(kMonthsFullWithDigits, &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_MONTH);

  CreditCard card = test::GetCreditCard();
  card.SetExpirationMonth(4);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"April (04)", field.value);
}

TEST_F(AutofillFieldFillerTest,
       FillSelectControlWithMonthNameAndDigits_French) {
  std::vector<const char*> kMonthsFullWithDigits = {
      "01 - JANVIER",
      "02 - FÉVRIER",
      "03 - MARS",
      "04 - AVRIL",
      "05 - MAI",
      "06 - JUIN",
      "07 - JUILLET",
      "08 - AOÛT",
      "09 - SEPTEMBRE",
      "10 - OCTOBRE",
      "11 - NOVEMBRE",
      "12 - DECEMBRE" /* Intentionally not including accent in DÉCEMBRE */,
  };
  AutofillField field;
  test::CreateTestSelectField(kMonthsFullWithDigits, &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_MONTH);

  CreditCard card = test::GetCreditCard();
  card.SetExpirationMonth(8);
  FieldFiller filler(/*app_locale=*/"fr-FR", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"08 - AOÛT", field.value);
  card.SetExpirationMonth(12);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"12 - DECEMBRE", field.value);
}

TEST_F(AutofillFieldFillerTest, FillSelectControlWithMonthName_French) {
  std::vector<const char*> kMonthsFrench = {"JANV", "FÉVR.", "MARS",
                                            "décembre"};
  AutofillField field;
  test::CreateTestSelectField(kMonthsFrench, &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_MONTH);

  CreditCard card = test::GetCreditCard();
  card.SetExpirationMonth(2);
  FieldFiller filler(/*app_locale=*/"fr-FR", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"FÉVR.", field.value);

  card.SetExpirationMonth(1);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"JANV", field.value);

  card.SetExpirationMonth(12);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"décembre", field.value);
}

TEST_F(AutofillFieldFillerTest,
       FillSelectControlWithNumericMonthSansLeadingZero) {
  std::vector<const char*> kMonthsNumeric = {
      "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
  };
  AutofillField field;
  test::CreateTestSelectField(kMonthsNumeric, &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_MONTH);

  CreditCard card = test::GetCreditCard();
  card.SetExpirationMonth(4);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"4", field.value);
}

TEST_F(AutofillFieldFillerTest, FillSelectControlWithTwoDigitCreditCardYear) {
  std::vector<const char*> kYears = {"12", "13", "14", "15",
                                     "16", "17", "18", "19"};
  AutofillField field;
  test::CreateTestSelectField(kYears, &field);
  field.set_heuristic_type(CREDIT_CARD_EXP_2_DIGIT_YEAR);

  CreditCard card = test::GetCreditCard();
  card.SetExpirationYear(2017);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"17", field.value);
}

TEST_F(AutofillFieldFillerTest, FillSelectControlWithCreditCardType) {
  std::vector<const char*> kCreditCardTypes = {"Visa", "Mastercard", "AmEx",
                                               "discover"};
  AutofillField field;
  test::CreateTestSelectField(kCreditCardTypes, &field);
  field.set_heuristic_type(CREDIT_CARD_TYPE);
  CreditCard card = test::GetCreditCard();
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);

  // Normal case:
  card.SetNumber(u"4111111111111111");  // Visa number.
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Visa", field.value);

  // Filling should be able to handle intervening whitespace:
  card.SetNumber(u"5555555555554444");  // MC number.
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Mastercard", field.value);

  // American Express is sometimes abbreviated as AmEx:
  card.SetNumber(u"378282246310005");  // Amex number.
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"AmEx", field.value);

  // Case insensitivity:
  card.SetNumber(u"6011111111111117");  // Discover number.
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"discover", field.value);
}

TEST_F(AutofillFieldFillerTest, FillMonthControl) {
  AutofillField field;
  field.form_control_type = "month";
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  field.set_heuristic_type(CREDIT_CARD_EXP_4_DIGIT_YEAR);

  // Try a month with two digits.
  CreditCard card = test::GetCreditCard();
  card.SetExpirationDateFromString(u"12/2017");
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"2017-12", field.value);

  // Try a month with a leading zero.
  card.SetExpirationDateFromString(u"03/2019");
  filler.FillFormField(field, &card, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"2019-03", field.value);
}

TEST_F(AutofillFieldFillerTest, FillStreetAddressTextArea) {
  AutofillField field;
  field.form_control_type = "textarea";
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  field.set_heuristic_type(ADDRESS_HOME_STREET_ADDRESS);

  std::u16string value = u"123 Fake St.\nApt. 42";
  address()->SetInfo(AutofillType(ADDRESS_HOME_STREET_ADDRESS), value, "en-US");
  filler.FillFormField(field, address(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(value, field.value);

  std::u16string ja_value = u"桜丘町26-1\nセルリアンタワー6階";
  address()->SetInfo(AutofillType(ADDRESS_HOME_STREET_ADDRESS), ja_value,
                     "ja-JP");
  address()->set_language_code("ja-JP");
  filler.FillFormField(field, address(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(ja_value, field.value);
}

TEST_F(AutofillFieldFillerTest, FillStreetAddressTextField) {
  AutofillField field;
  AutofillQueryResponse::FormSuggestion::FieldSuggestion::FieldPrediction
      prediction;
  field.form_control_type = "text";
  prediction.set_type(ADDRESS_HOME_STREET_ADDRESS);
  field.set_server_predictions({prediction});
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);

  std::u16string value = u"123 Fake St.\nApt. 42";
  address()->SetInfo(AutofillType(ADDRESS_HOME_STREET_ADDRESS), value, "en-US");
  filler.FillFormField(field, address(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"123 Fake St., Apt. 42", field.value);

  std::u16string ja_value = u"桜丘町26-1\nセルリアンタワー6階";
  address()->SetInfo(AutofillType(ADDRESS_HOME_STREET_ADDRESS), ja_value,
                     "ja-JP");
  address()->set_language_code("ja-JP");
  filler.FillFormField(field, address(), &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"桜丘町26-1セルリアンタワー6階", field.value);
}

TEST_F(AutofillFieldFillerTest, FillCreditCardNumberWithoutSplits) {
  // Case 1: card number without any split.
  AutofillField cc_number_full;
  cc_number_full.set_heuristic_type(CREDIT_CARD_NUMBER);

  credit_card()->SetNumber(u"41111111111111111");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(cc_number_full, credit_card(), &cc_number_full,
                       /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify that full card-number shall get filled properly.
  EXPECT_EQ(u"41111111111111111", cc_number_full.value);
  EXPECT_EQ(0U, cc_number_full.credit_card_number_offset());
}

TEST_F(AutofillFieldFillerTest, FillCreditCardNumberWithEqualSizeSplits) {
  // Case 2: card number broken up into four equal groups, of length 4.
  CreditCardTestCase test;
  test.card_number_ = u"5187654321098765";
  test.total_splits_ = 4;
  int splits[] = {4, 4, 4, 4};
  test.splits_ = std::vector<int>(splits, splits + base::size(splits));
  test.expected_results_ = {u"5187", u"6543", u"2109", u"8765"};

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  for (size_t i = 0; i < test.total_splits_; ++i) {
    AutofillField cc_number_part;
    cc_number_part.set_heuristic_type(CREDIT_CARD_NUMBER);
    cc_number_part.max_length = test.splits_[i];
    cc_number_part.set_credit_card_number_offset(4 * i);

    // Fill with a card-number; should fill just the card_number_part.
    credit_card()->SetNumber(test.card_number_);
    filler.FillFormField(cc_number_part, credit_card(), &cc_number_part,
                         /*cvc=*/std::u16string(),
                         mojom::RendererFormDataAction::kFill);

    // Verify for expected results.
    EXPECT_EQ(test.expected_results_[i],
              cc_number_part.value.substr(0, cc_number_part.max_length));
    EXPECT_EQ(4 * i, cc_number_part.credit_card_number_offset());
  }

  // Verify that full card-number shall get fill properly as well.
  AutofillField cc_number_full;
  cc_number_full.set_heuristic_type(CREDIT_CARD_NUMBER);

  credit_card()->SetNumber(test.card_number_);
  filler.FillFormField(cc_number_full, credit_card(), &cc_number_full,
                       /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify for expected results.
  EXPECT_EQ(test.card_number_, cc_number_full.value);
}

TEST_F(AutofillFieldFillerTest, FillCreditCardNumberWithUnequalSizeSplits) {
  // Case 3: card with 15 digits number, broken up into three unequal groups, of
  // lengths 4, 6, and 5.
  CreditCardTestCase test;
  test.card_number_ = u"423456789012345";
  test.total_splits_ = 3;
  int splits[] = {4, 6, 5};
  test.splits_ = std::vector<int>(splits, splits + base::size(splits));
  test.expected_results_ = {u"4234", u"567890", u"12345"};

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  // Start executing test cases to verify parts and full credit card number.
  for (size_t i = 0; i < test.total_splits_; ++i) {
    AutofillField cc_number_part;
    cc_number_part.set_heuristic_type(CREDIT_CARD_NUMBER);
    cc_number_part.max_length = test.splits_[i];
    cc_number_part.set_credit_card_number_offset(GetNumberOffset(i, test));

    // Fill with a card-number; should fill just the card_number_part.
    credit_card()->SetNumber(test.card_number_);
    filler.FillFormField(cc_number_part, credit_card(), &cc_number_part,
                         /*cvc=*/std::u16string(),
                         mojom::RendererFormDataAction::kFill);

    // Verify for expected results.
    EXPECT_EQ(test.expected_results_[i],
              cc_number_part.value.substr(0, cc_number_part.max_length));
    EXPECT_EQ(GetNumberOffset(i, test),
              cc_number_part.credit_card_number_offset());
  }

  // Verify that full card-number shall get fill properly as well.
  AutofillField cc_number_full;
  cc_number_full.set_heuristic_type(CREDIT_CARD_NUMBER);
  credit_card()->SetNumber(test.card_number_);
  filler.FillFormField(cc_number_full, credit_card(), &cc_number_full,
                       /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);

  // Verify for expected results.
  EXPECT_EQ(test.card_number_, cc_number_full.value);
}

TEST_F(AutofillFieldFillerTest, FindShortestSubstringMatchInSelect) {
  std::vector<const char*> kCountries = {"États-Unis", "Canada"};
  AutofillField field;
  test::CreateTestSelectField(kCountries, &field);
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);

  // Case 1: Exact match
  int ret =
      FieldFiller::FindShortestSubstringMatchInSelect(u"Canada", false, &field);
  EXPECT_EQ(1, ret);

  // Case 2: Case-insensitive
  ret =
      FieldFiller::FindShortestSubstringMatchInSelect(u"CANADA", false, &field);
  EXPECT_EQ(1, ret);

  // Case 3: Proper substring
  ret =
      FieldFiller::FindShortestSubstringMatchInSelect(u"États", false, &field);
  EXPECT_EQ(0, ret);

  // Case 4: Accent-insensitive
  ret = FieldFiller::FindShortestSubstringMatchInSelect(u"Etats-Unis", false,
                                                        &field);
  EXPECT_EQ(0, ret);

  // Case 5: Whitespace-insensitive
  ret = FieldFiller::FindShortestSubstringMatchInSelect(u"Ca na da", true,
                                                        &field);
  EXPECT_EQ(1, ret);

  // Case 6: No match (whitespace-sensitive)
  ret = FieldFiller::FindShortestSubstringMatchInSelect(u"Ca Na Da", false,
                                                        &field);
  EXPECT_EQ(-1, ret);

  // Case 7: No match (not present)
  ret =
      FieldFiller::FindShortestSubstringMatchInSelect(u"Canadia", true, &field);
  EXPECT_EQ(-1, ret);
}

// Tests that text state fields are filled correctly depending on their
// maxlength attribute value.
struct FillStateTextTestCase {
  HtmlFieldType field_type;
  size_t field_max_length;
  std::u16string value_to_fill;
  std::u16string expected_value;
  bool should_fill;
};

class AutofillStateTextTest
    : public testing::TestWithParam<FillStateTextTestCase> {
 public:
  AutofillStateTextTest() { CountryNames::SetLocaleString("en-US"); }
};

TEST_P(AutofillStateTextTest, FillStateText) {
  auto test_case = GetParam();
  AutofillField field;
  field.SetHtmlType(test_case.field_type, HtmlFieldMode());
  field.max_length = test_case.field_max_length;

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  AutofillProfile address = test::GetFullProfile();
  address.SetRawInfo(ADDRESS_HOME_STATE, test_case.value_to_fill);
  bool has_filled = filler.FillFormField(field, &address, &field,
                                         /*cvc=*/std::u16string(),
                                         mojom::RendererFormDataAction::kFill);

  EXPECT_EQ(test_case.should_fill, has_filled);
  EXPECT_EQ(test_case.expected_value, field.value);
}

INSTANTIATE_TEST_SUITE_P(
    AutofillFieldFillerTest,
    AutofillStateTextTest,
    testing::Values(
        // Filling a state to a text field with the default maxlength value
        // should
        // fill the state value as is.
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, /* default value */ 0,
                              u"New York", u"New York", true},
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, /* default value */ 0,
                              u"NY", u"NY", true},
        // Filling a state to a text field with a maxlength value equal to the
        // value's length should fill the state value as is.
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, 8, u"New York",
                              u"New York", true},
        // Filling a state to a text field with a maxlength value lower than the
        // value's length but higher than the value's abbreviation should fill
        // the state abbreviation.
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, 2, u"New York", u"NY",
                              true},
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, 2, u"NY", u"NY", true},
        // Filling a state to a text field with a maxlength value lower than the
        // value's length and the value's abbreviation should not fill at all.
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, 1, u"New York", u"",
                              false},
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, 1, u"NY", u"", false},
        // Filling a state to a text field with a maxlength value lower than the
        // value's length and that has no associated abbreviation should not
        // fill at all.
        FillStateTextTestCase{HTML_TYPE_ADDRESS_LEVEL1, 3, u"Quebec", u"",
                              false}));

// Tests that the correct option is chosen in the selection box when one of the
// options exactly matches the phone country code.
TEST_F(AutofillFieldFillerTest,
       FillSelectControlPhoneCountryCodeWithExactMatch) {
  base::test::ScopedFeatureList enabled;
  enabled.InitAndEnableFeature(
      features::kAutofillEnableAugmentedPhoneCountryCode);

  std::vector<const char*> kPhoneCountryCode = {"91", "1", "20", "49"};
  AutofillField field;
  test::CreateTestSelectField(kPhoneCountryCode, &field);
  field.set_heuristic_type(PHONE_HOME_COUNTRY_CODE);

  AutofillProfile address;
  address.SetRawInfo(PHONE_HOME_WHOLE_NUMBER, u"+15145554578");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"1", field.value);
}

// Tests that the correct option is chosen in the selection box when the options
// are preceded by a plus sign and the field is of |PHONE_HOME_COUNTRY_CODE|
// type.
TEST_F(AutofillFieldFillerTest,
       FillSelectControlPhoneCountryCodePrecededByPlus) {
  base::test::ScopedFeatureList enabled;
  enabled.InitAndEnableFeature(
      features::kAutofillEnableAugmentedPhoneCountryCode);

  std::vector<const char*> kPhoneCountryCode = {"+91", "+1", "+20", "+49"};
  AutofillField field;
  test::CreateTestSelectField(kPhoneCountryCode, &field);
  field.set_heuristic_type(PHONE_HOME_COUNTRY_CODE);

  AutofillProfile address;
  address.SetRawInfo(PHONE_HOME_WHOLE_NUMBER, u"+918890888888");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"+91", field.value);
}

// Tests that the correct option is chosen in the selection box when the options
// are preceded by a '00' and the field is of |PHONE_HOME_COUNTRY_CODE|
// type.
TEST_F(AutofillFieldFillerTest,
       FillSelectControlPhoneCountryCodePrecededByDoubleZeros) {
  base::test::ScopedFeatureList enabled;
  enabled.InitAndEnableFeature(
      features::kAutofillEnableAugmentedPhoneCountryCode);

  std::vector<const char*> kPhoneCountryCode = {"0091", "001", "0020", "0049"};
  AutofillField field;
  test::CreateTestSelectField(kPhoneCountryCode, &field);
  field.set_heuristic_type(PHONE_HOME_COUNTRY_CODE);

  AutofillProfile address;
  address.SetRawInfo(PHONE_HOME_WHOLE_NUMBER, u"+918890888888");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"0091", field.value);
}

// Tests that the correct option is chosen in the selection box when the options
// are composed of the country code and the country name.
TEST_F(AutofillFieldFillerTest, FillSelectControlAugmentedPhoneCountryCode) {
  base::test::ScopedFeatureList enabled;
  enabled.InitAndEnableFeature(
      features::kAutofillEnableAugmentedPhoneCountryCode);

  std::vector<const char*> kPhoneCountryCode = {
      "Please select an option", "+91 (India)", "+1 (United States)",
      "+20 (Egypt)", "+49 (Germany)"};
  AutofillField field;
  test::CreateTestSelectField(kPhoneCountryCode, &field);
  field.set_heuristic_type(PHONE_HOME_COUNTRY_CODE);

  AutofillProfile address;
  address.SetRawInfo(PHONE_HOME_WHOLE_NUMBER, u"+49151669087345");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"+49 (Germany)", field.value);
}

// Tests that the correct option is chosen in the selection box when the options
// are composed of the country code having whitespace and the country name.
TEST_F(AutofillFieldFillerTest,
       FillSelectControlAugmentedPhoneCountryCodeWithWhiteSpaces) {
  base::test::ScopedFeatureList enabled;
  enabled.InitAndEnableFeature(
      features::kAutofillEnableAugmentedPhoneCountryCode);

  std::vector<const char*> kPhoneCountryCode = {
      "Please select an option", "(00 91) India", "(00 1) United States",
      "(00 20) Egypt", "(00 49) Germany"};
  AutofillField field;
  test::CreateTestSelectField(kPhoneCountryCode, &field);
  field.set_heuristic_type(PHONE_HOME_COUNTRY_CODE);

  AutofillProfile address;
  address.SetRawInfo(PHONE_HOME_WHOLE_NUMBER, u"+49151669087345");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"(00 49) Germany", field.value);
}

// Tests that the correct option is chosen in the selection box when the options
// are composed of the country code that is preceded by '00' and the country
// name.
TEST_F(AutofillFieldFillerTest,
       FillSelectControlAugmentedPhoneCountryCodeHavingDoubleZeros) {
  base::test::ScopedFeatureList enabled;
  enabled.InitAndEnableFeature(
      features::kAutofillEnableAugmentedPhoneCountryCode);

  std::vector<const char*> kPhoneCountryCode = {
      "Please select an option", "(0091) India", "(001) United States",
      "(0020) Egypt", "(0049) Germany"};
  AutofillField field;
  test::CreateTestSelectField(kPhoneCountryCode, &field);
  field.set_heuristic_type(PHONE_HOME_COUNTRY_CODE);

  AutofillProfile address;
  address.SetRawInfo(PHONE_HOME_WHOLE_NUMBER, u"+49151669087345");
  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"(0049) Germany", field.value);
}

// Tests that the abbreviated state names are selected correctly.
TEST_F(AutofillFieldFillerTest, FillSelectAbbreviatedState) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  test::PopulateAlternativeStateNameMapForTesting();
  std::vector<const char*> kState = {"BA", "BB", "BC", "BY"};

  AutofillField field;
  test::CreateTestSelectField(kState, &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"Bavaria");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"DE");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"BY", field.value);
}

// Tests that the localized state names are selected correctly.
TEST_F(AutofillFieldFillerTest, FillSelectLocalizedState) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  test::PopulateAlternativeStateNameMapForTesting();
  std::vector<const char*> kState = {"Bayern", "Berlin", "Brandenburg",
                                     "Bremen"};

  AutofillField field;
  test::CreateTestSelectField(kState, &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"Bavaria");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"DE");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Bayern", field.value);
}

// Tests that the state names are selected correctly when the state name exists
// as a substring in the selection options.
TEST_F(AutofillFieldFillerTest, FillSelectLocalizedStateSubstring) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  test::PopulateAlternativeStateNameMapForTesting();
  std::vector<const char*> kState = {"Bavaria Has Munich", "Berlin has Berlin"};

  AutofillField field;
  test::CreateTestSelectField(kState, &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"Bavaria");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"DE");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Bavaria Has Munich", field.value);
}

// Tests that the state abbreviations are filled in the text field when the
// field length is limited.
TEST_F(AutofillFieldFillerTest, FillStateAbbreviationInTextField) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  test::PopulateAlternativeStateNameMapForTesting();

  AutofillField field;
  test::CreateTestFormField("State", "state", "", "text", &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);
  field.max_length = 4;

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"Bavaria");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"DE");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"BY", field.value);
}

// Tests that the state names are selected correctly even though the state
// value saved in the address is not recognized by the AlternativeStateNameMap.
TEST_F(AutofillFieldFillerTest, FillStateFieldWithSavedValueInProfile) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  test::PopulateAlternativeStateNameMapForTesting();
  std::vector<const char*> kState = {"Bavari", "Berlin", "Lower Saxony"};

  AutofillField field;
  test::CreateTestSelectField(kState, &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"Bavari");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"DE");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Bavari", field.value);
}

// Tests that Autofill does not wrongly fill the state when the appropriate
// state is not in the list of selection options given that the abbreviation is
// saved in the profile.
TEST_F(AutofillFieldFillerTest, FillStateFieldWhenStateIsNotInOptions) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  test::PopulateAlternativeStateNameMapForTesting(
      "US", "Colorado",
      {{.canonical_name = "Colorado",
        .abbreviations = {"CO"},
        .alternative_names = {}}});
  std::vector<const char*> kState = {"Connecticut", "California"};

  AutofillField field;
  test::CreateTestSelectField(kState, &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"CO");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"US");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"", field.value);
}

// Tests that Autofill uses the static states data of US as a fallback mechanism
// for filling when |AlternativeStateNameMap| is not populated.
TEST_F(AutofillFieldFillerTest,
       FillStateFieldWhenAlternativeStateNameMapIsNotPopulated) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  std::vector<const char*> kState = {"Colorado", "Connecticut", "California"};

  AutofillField field;
  test::CreateTestSelectField(kState, &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"CO");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"US");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"Colorado", field.value);
}

// Tests that Autofill fills upper case abbreviation in the input field when
// field length is limited.
TEST_F(AutofillFieldFillerTest, FillUpperCaseAbbreviationInStateTextField) {
  base::test::ScopedFeatureList feature;
  feature.InitAndEnableFeature(features::kAutofillUseAlternativeStateNameMap);

  test::ClearAlternativeStateNameMapForTesting();
  test::PopulateAlternativeStateNameMapForTesting("DE", "Bavaria",
                                                  {{.canonical_name = "Bavaria",
                                                    .abbreviations = {"by"},
                                                    .alternative_names = {}}});

  AutofillField field;
  test::CreateTestFormField("State", "state", "", "text", &field);
  field.set_heuristic_type(ADDRESS_HOME_STATE);
  field.max_length = 4;

  AutofillProfile address;
  address.SetRawInfo(ADDRESS_HOME_STATE, u"Bavaria");
  address.SetRawInfo(ADDRESS_HOME_COUNTRY, u"DE");

  FieldFiller filler(/*app_locale=*/"en-US", /*address_normalizer=*/nullptr);
  filler.FillFormField(field, &address, &field, /*cvc=*/std::u16string(),
                       mojom::RendererFormDataAction::kFill);
  EXPECT_EQ(u"BY", field.value);
}

}  // namespace autofill
