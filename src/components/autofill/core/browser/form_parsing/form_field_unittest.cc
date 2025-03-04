// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "components/autofill/core/browser/autofill_field.h"
#include "components/autofill/core/browser/form_parsing/form_field.h"
#include "components/autofill/core/browser/form_structure.h"
#include "components/autofill/core/common/autofill_features.h"
#include "testing/gtest/include/gtest/gtest.h"

using autofill::features::kAutofillFixFillableFieldTypes;

namespace autofill {

namespace {
FieldRendererId MakeFieldRendererId() {
  static uint64_t id_counter_ = 0;
  return FieldRendererId(++id_counter_);
}

// Sets both the field label and parseable label to |label|.
void SetFieldLabels(AutofillField* field, const std::u16string& label) {
  field->label = label;
  field->set_parseable_label(label);
}

}  // namespace

TEST(FormFieldTest, Match) {
  AutofillField field;

  // Empty strings match.
  EXPECT_TRUE(FormField::Match(&field, std::u16string(), MATCH_LABEL));

  // Empty pattern matches non-empty string.
  SetFieldLabels(&field, u"a");
  EXPECT_TRUE(FormField::Match(&field, std::u16string(), MATCH_LABEL));

  // Strictly empty pattern matches empty string.
  SetFieldLabels(&field, u"");
  EXPECT_TRUE(FormField::Match(&field, u"^$", MATCH_LABEL));

  // Strictly empty pattern does not match non-empty string.
  SetFieldLabels(&field, u"a");
  EXPECT_FALSE(FormField::Match(&field, u"^$", MATCH_LABEL));

  // Non-empty pattern doesn't match empty string.
  SetFieldLabels(&field, u"");
  EXPECT_FALSE(FormField::Match(&field, u"a", MATCH_LABEL));

  // Beginning of line.
  SetFieldLabels(&field, u"head_tail");
  EXPECT_TRUE(FormField::Match(&field, u"^head", MATCH_LABEL));
  EXPECT_FALSE(FormField::Match(&field, u"^tail", MATCH_LABEL));

  // End of line.
  SetFieldLabels(&field, u"head_tail");
  EXPECT_FALSE(FormField::Match(&field, u"head$", MATCH_LABEL));
  EXPECT_TRUE(FormField::Match(&field, u"tail$", MATCH_LABEL));

  // Exact.
  SetFieldLabels(&field, u"head_tail");
  EXPECT_FALSE(FormField::Match(&field, u"^head$", MATCH_LABEL));
  EXPECT_FALSE(FormField::Match(&field, u"^tail$", MATCH_LABEL));
  EXPECT_TRUE(FormField::Match(&field, u"^head_tail$", MATCH_LABEL));

  // Escaped dots.
  SetFieldLabels(&field, u"m.i.");
  // Note: This pattern is misleading as the "." characters are wild cards.
  EXPECT_TRUE(FormField::Match(&field, u"m.i.", MATCH_LABEL));
  EXPECT_TRUE(FormField::Match(&field, u"m\\.i\\.", MATCH_LABEL));
  SetFieldLabels(&field, u"mXiX");
  EXPECT_TRUE(FormField::Match(&field, u"m.i.", MATCH_LABEL));
  EXPECT_FALSE(FormField::Match(&field, u"m\\.i\\.", MATCH_LABEL));

  // Repetition.
  SetFieldLabels(&field, u"headtail");
  EXPECT_TRUE(FormField::Match(&field, u"head.*tail", MATCH_LABEL));
  SetFieldLabels(&field, u"headXtail");
  EXPECT_TRUE(FormField::Match(&field, u"head.*tail", MATCH_LABEL));
  SetFieldLabels(&field, u"headXXXtail");
  EXPECT_TRUE(FormField::Match(&field, u"head.*tail", MATCH_LABEL));
  SetFieldLabels(&field, u"headtail");
  EXPECT_FALSE(FormField::Match(&field, u"head.+tail", MATCH_LABEL));
  SetFieldLabels(&field, u"headXtail");
  EXPECT_TRUE(FormField::Match(&field, u"head.+tail", MATCH_LABEL));
  SetFieldLabels(&field, u"headXXXtail");
  EXPECT_TRUE(FormField::Match(&field, u"head.+tail", MATCH_LABEL));

  // Alternation.
  SetFieldLabels(&field, u"head_tail");
  EXPECT_TRUE(FormField::Match(&field, u"head|other", MATCH_LABEL));
  EXPECT_TRUE(FormField::Match(&field, u"tail|other", MATCH_LABEL));
  EXPECT_FALSE(FormField::Match(&field, u"bad|good", MATCH_LABEL));

  // Case sensitivity.
  SetFieldLabels(&field, u"xxxHeAd_tAiLxxx");
  EXPECT_TRUE(FormField::Match(&field, u"head_tail", MATCH_LABEL));

  // Word boundaries.
  SetFieldLabels(&field, u"contains word:");
  EXPECT_TRUE(FormField::Match(&field, u"\\bword\\b", MATCH_LABEL));
  EXPECT_FALSE(FormField::Match(&field, u"\\bcon\\b", MATCH_LABEL));
  // Make sure the circumflex in 'crêpe' is not treated as a word boundary.
  field.label = u"crêpe";
  EXPECT_FALSE(FormField::Match(&field, u"\\bcr\\b", MATCH_LABEL));
}

// Test that we ignore checkable elements.
TEST(FormFieldTest, ParseFormFields) {
  std::vector<std::unique_ptr<AutofillField>> fields;
  FormFieldData field_data;
  field_data.form_control_type = "text";

  field_data.check_status = FormFieldData::CheckStatus::kCheckableButUnchecked;
  field_data.label = u"Is PO Box";
  field_data.unique_renderer_id = MakeFieldRendererId();
  fields.push_back(std::make_unique<AutofillField>(field_data));

  // Does not parse since there are only field and it's checkable.
  // An empty page_language means the language is unknown and patterns of all
  // languages are used.
  EXPECT_TRUE(
      FormField::ParseFormFields(fields, LanguageCode(""), true).empty());

  // reset |is_checkable| to false.
  field_data.check_status = FormFieldData::CheckStatus::kNotCheckable;
  field_data.label = u"Address line1";
  field_data.unique_renderer_id = MakeFieldRendererId();
  fields.push_back(std::make_unique<AutofillField>(field_data));

  // Parse a single address line 1 field.
  ASSERT_EQ(0u,
            FormField::ParseFormFields(fields, LanguageCode(""), true).size());

  // Parses address line 1 and 2.
  field_data.label = u"Address line2";
  field_data.unique_renderer_id = MakeFieldRendererId();
  fields.push_back(std::make_unique<AutofillField>(field_data));

  // An empty page_language means the language is unknown and patterns of
  // all languages are used.
  ASSERT_EQ(0u,
            FormField::ParseFormFields(fields, LanguageCode(""), true).size());
}

// Test that the minimum number of required fields for the heuristics considers
// whether a field is actually fillable.
TEST(FormFieldTest, ParseFormFieldEnforceMinFillableFields) {
  std::vector<std::unique_ptr<AutofillField>> fields;
  FormFieldData field_data;
  field_data.form_control_type = "text";

  field_data.label = u"Address line 1";
  field_data.unique_renderer_id = MakeFieldRendererId();
  fields.push_back(std::make_unique<AutofillField>(field_data));

  field_data.label = u"Address line 2";
  field_data.unique_renderer_id = MakeFieldRendererId();
  fields.push_back(std::make_unique<AutofillField>(field_data));

  // Don't parse forms with 2 fields.
  // An empty page_language means the language is unknown and patterns of all
  // languages are used.
  EXPECT_EQ(0u,
            FormField::ParseFormFields(fields, LanguageCode(""), true).size());

  field_data.label = u"Search";
  field_data.unique_renderer_id = MakeFieldRendererId();
  fields.push_back(std::make_unique<AutofillField>(field_data));

  // Before the fix in kAutofillFixFillableFieldTypes, we would parse the form
  // now, although a search field is not fillable.
  {
    base::test::ScopedFeatureList feature_list;
    feature_list.InitAndDisableFeature(kAutofillFixFillableFieldTypes);
    // An empty page_language means the language is unknown and patterns of all
    // languages are used.
    EXPECT_EQ(
        3u, FormField::ParseFormFields(fields, LanguageCode(""), true).size());
  }

  // With the fix, we don't parse the form because search fields are not
  // fillable (therefore, the form has only 2 fillable fields).
  {
    base::test::ScopedFeatureList feature_list;
    feature_list.InitAndEnableFeature(kAutofillFixFillableFieldTypes);
    // An empty page_language means the language is unknown and patterns of all
    // languages are used.
    const FieldCandidatesMap field_candidates_map =
        FormField::ParseFormFields(fields, LanguageCode(""), true);
    EXPECT_EQ(
        0u, FormField::ParseFormFields(fields, LanguageCode(""), true).size());
  }
}

// Test that the parseable label is used when the feature is enabled.
TEST(FormFieldTest, TestParseableLabels) {
  FormFieldData field_data;
  field_data.form_control_type = "text";

  field_data.label = u"not a parseable label";
  field_data.unique_renderer_id = MakeFieldRendererId();
  auto autofill_field = std::make_unique<AutofillField>(field_data);
  autofill_field->set_parseable_label(u"First Name");
  {
    base::test::ScopedFeatureList feature_list;
    feature_list.InitAndEnableFeature(
        features::kAutofillEnableSupportForParsingWithSharedLabels);
    EXPECT_TRUE(
        FormField::Match(autofill_field.get(), u"First Name", MATCH_LABEL));
  }
  {
    base::test::ScopedFeatureList feature_list;
    feature_list.InitAndDisableFeature(
        features::kAutofillEnableSupportForParsingWithSharedLabels);
    EXPECT_FALSE(
        FormField::Match(autofill_field.get(), u"First Name", MATCH_LABEL));
  }
}
}  // namespace autofill
