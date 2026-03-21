// MultiLineEdit — multi-line text widget with selection and clipboard.

#include "../Precompiled.h"
#include "MultiLineEdit.h"
#include "../UI/Text.h"
#include "../UI/UI.h"
#include "../UI/UIEvents.h"
#include "../Input/Input.h"
#include "../Input/InputEvents.h"
#include "../Core/Context.h"

namespace Urho3D
{

extern const char* UI_CATEGORY;

MultiLineEdit::MultiLineEdit(Context* context) : BorderImage(context)
{
    SetEnabled(true);
    SetFocusMode(FM_FOCUSABLE_DEFOCUSABLE);
    SetClipChildren(true);

    textElement_ = CreateChild<Text>("MLE_Text");
    textElement_->SetInternal(true);
    textElement_->SetWordwrap(true);

    cursor_ = CreateChild<BorderImage>("MLE_Cursor");
    cursor_->SetInternal(true);
    cursor_->SetEnabled(false);
    cursor_->SetFixedSize(1, 0);
    cursor_->SetVisible(false);

    SubscribeToEvent(this, E_FOCUSED, URHO3D_HANDLER(MultiLineEdit, HandleFocused));
    SubscribeToEvent(this, E_DEFOCUSED, URHO3D_HANDLER(MultiLineEdit, HandleDefocused));
}

MultiLineEdit::~MultiLineEdit() = default;

void MultiLineEdit::RegisterObject(Context* context)
{
    context->RegisterFactory<MultiLineEdit>(UI_CATEGORY);
    URHO3D_COPY_BASE_ATTRIBUTES(BorderImage);
    URHO3D_UPDATE_ATTRIBUTE_DEFAULT_VALUE("Clip Children", true);
    URHO3D_UPDATE_ATTRIBUTE_DEFAULT_VALUE("Is Enabled", true);
    URHO3D_UPDATE_ATTRIBUTE_DEFAULT_VALUE("Focus Mode", FM_FOCUSABLE_DEFOCUSABLE);
}

void MultiLineEdit::ApplyAttributes()
{
    BorderImage::ApplyAttributes();
    UpdateText();
    UpdateCursor();
}

void MultiLineEdit::Update(float timeStep)
{
    if (cursorBlinkRate_ > 0.0f)
        cursorBlinkTimer_ += timeStep;

    // Toggle cursor visibility based on blink
    if (HasFocus() && !readOnly_)
    {
        float blinkPeriod = 1.0f / cursorBlinkRate_;
        bool visible = fmodf(cursorBlinkTimer_, blinkPeriod) < blinkPeriod * 0.5f;
        cursor_->SetVisible(visible);
    }
    else
    {
        cursor_->SetVisible(false);
    }
}

void MultiLineEdit::OnClickBegin(const IntVector2& position, const IntVector2& screenPosition,
    MouseButton button, MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor)
{
    if (button == MOUSEB_LEFT)
    {
        i32 pos = GetCharIndex(position);
        if (pos != NINDEX)
        {
            SetCursorPosition(pos);
            textElement_->ClearSelection();
        }
    }
}

void MultiLineEdit::OnDoubleClick(const IntVector2& position, const IntVector2& screenPosition,
    MouseButton button, MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor)
{
    if (button == MOUSEB_LEFT)
        textElement_->SetSelection(0);  // Select all on double-click
}

void MultiLineEdit::OnDragBegin(const IntVector2& position, const IntVector2& screenPosition,
    MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor)
{
    UIElement::OnDragBegin(position, screenPosition, buttons, qualifiers, cursor);
    dragBeginCursor_ = GetCharIndex(position);
}

void MultiLineEdit::OnDragMove(const IntVector2& position, const IntVector2& screenPosition,
    const IntVector2& deltaPos, MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor)
{
    i32 start = dragBeginCursor_;
    i32 current = GetCharIndex(position);
    if (start != NINDEX && current != NINDEX)
    {
        if (start < current)
            textElement_->SetSelection(start, current - start);
        else
            textElement_->SetSelection(current, start - current);
        SetCursorPosition(current);
    }
}

void MultiLineEdit::OnKey(Key key, MouseButtonFlags buttons, QualifierFlags qualifiers)
{
    bool ctrl = (qualifiers & QUAL_CTRL) != 0;
    bool shift = (qualifiers & QUAL_SHIFT) != 0;

    switch (key)
    {
    case KEY_C:
        if (ctrl)
        {
            // Copy selected text to clipboard
            String selected = GetSelectedText();
            if (!selected.Empty())
                GetSubsystem<UI>()->SetClipboardText(selected);
        }
        break;

    case KEY_A:
        if (ctrl)
            textElement_->SetSelection(0);  // Select all
        break;

    case KEY_V:
        if (ctrl && !readOnly_)
        {
            const String& clip = GetSubsystem<UI>()->GetClipboardText();
            if (!clip.Empty())
            {
                // Insert at cursor
                i32 pos = cursorPosition_;
                text_ = text_.SubstringUTF8(0, pos) + clip + text_.SubstringUTF8(pos);
                UpdateText();
                SetCursorPosition(pos + clip.LengthUTF8());
            }
        }
        break;

    case KEY_X:
        if (ctrl)
        {
            String selected = GetSelectedText();
            if (!selected.Empty())
            {
                GetSubsystem<UI>()->SetClipboardText(selected);
                if (!readOnly_)
                {
                    i32 start = textElement_->GetSelectionStart();
                    i32 length = textElement_->GetSelectionLength();
                    text_ = text_.SubstringUTF8(0, start) + text_.SubstringUTF8(start + length);
                    textElement_->ClearSelection();
                    UpdateText();
                    SetCursorPosition(start);
                }
            }
        }
        break;

    case KEY_LEFT:
        if (shift && !textElement_->GetSelectionLength())
            dragBeginCursor_ = cursorPosition_;
        if (cursorPosition_ > 0)
        {
            if (textElement_->GetSelectionLength() && !shift)
                SetCursorPosition(textElement_->GetSelectionStart());
            else
                SetCursorPosition(cursorPosition_ - 1);
        }
        if (shift)
        {
            i32 start = dragBeginCursor_;
            i32 current = cursorPosition_;
            if (start < current)
                textElement_->SetSelection(start, current - start);
            else
                textElement_->SetSelection(current, start - current);
        }
        else
            textElement_->ClearSelection();
        break;

    case KEY_RIGHT:
        if (shift && !textElement_->GetSelectionLength())
            dragBeginCursor_ = cursorPosition_;
        if (cursorPosition_ < text_.LengthUTF8())
        {
            if (textElement_->GetSelectionLength() && !shift)
                SetCursorPosition(textElement_->GetSelectionStart() + textElement_->GetSelectionLength());
            else
                SetCursorPosition(cursorPosition_ + 1);
        }
        if (shift)
        {
            i32 start = dragBeginCursor_;
            i32 current = cursorPosition_;
            if (start < current)
                textElement_->SetSelection(start, current - start);
            else
                textElement_->SetSelection(current, start - current);
        }
        else
            textElement_->ClearSelection();
        break;

    case KEY_UP:
    {
        if (shift && !textElement_->GetSelectionLength())
            dragBeginCursor_ = cursorPosition_;
        Vector2 curPos = textElement_->GetCharPosition(cursorPosition_);
        int rowHeight = textElement_->GetRowHeight();
        if (rowHeight > 0)
        {
            IntVector2 above(static_cast<int>(curPos.x_), static_cast<int>(curPos.y_) - rowHeight);
            IntVector2 screenPos = textElement_->ElementToScreen(above);
            IntVector2 localPos = ScreenToElement(screenPos);
            i32 idx = GetCharIndex(localPos);
            if (idx != NINDEX)
                SetCursorPosition(idx);
        }
        if (shift)
        {
            i32 start = dragBeginCursor_;
            i32 current = cursorPosition_;
            if (start < current)
                textElement_->SetSelection(start, current - start);
            else
                textElement_->SetSelection(current, start - current);
        }
        else
            textElement_->ClearSelection();
        break;
    }

    case KEY_DOWN:
    {
        if (shift && !textElement_->GetSelectionLength())
            dragBeginCursor_ = cursorPosition_;
        Vector2 curPos = textElement_->GetCharPosition(cursorPosition_);
        int rowHeight = textElement_->GetRowHeight();
        if (rowHeight > 0)
        {
            IntVector2 below(static_cast<int>(curPos.x_), static_cast<int>(curPos.y_) + rowHeight);
            IntVector2 screenPos = textElement_->ElementToScreen(below);
            IntVector2 localPos = ScreenToElement(screenPos);
            i32 idx = GetCharIndex(localPos);
            if (idx != NINDEX)
                SetCursorPosition(idx);
        }
        if (shift)
        {
            i32 start = dragBeginCursor_;
            i32 current = cursorPosition_;
            if (start < current)
                textElement_->SetSelection(start, current - start);
            else
                textElement_->SetSelection(current, start - current);
        }
        else
            textElement_->ClearSelection();
        break;
    }

    case KEY_HOME:
        if (shift && !textElement_->GetSelectionLength())
            dragBeginCursor_ = cursorPosition_;
        SetCursorPosition(0);
        if (shift)
        {
            i32 start = dragBeginCursor_;
            i32 current = cursorPosition_;
            if (start < current)
                textElement_->SetSelection(start, current - start);
            else
                textElement_->SetSelection(current, start - current);
        }
        else
            textElement_->ClearSelection();
        break;

    case KEY_END:
        if (shift && !textElement_->GetSelectionLength())
            dragBeginCursor_ = cursorPosition_;
        SetCursorPosition(text_.LengthUTF8());
        if (shift)
        {
            i32 start = dragBeginCursor_;
            i32 current = cursorPosition_;
            if (start < current)
                textElement_->SetSelection(start, current - start);
            else
                textElement_->SetSelection(current, start - current);
        }
        else
            textElement_->ClearSelection();
        break;

    case KEY_BACKSPACE:
        if (!readOnly_ && cursorPosition_ > 0)
        {
            i32 pos = cursorPosition_ - 1;
            text_ = text_.SubstringUTF8(0, pos) + text_.SubstringUTF8(pos + 1);
            UpdateText();
            SetCursorPosition(pos);
        }
        break;

    case KEY_DELETE:
        if (!readOnly_ && cursorPosition_ < text_.LengthUTF8())
        {
            text_ = text_.SubstringUTF8(0, cursorPosition_) + text_.SubstringUTF8(cursorPosition_ + 1);
            UpdateText();
        }
        break;

    case KEY_RETURN:
    case KEY_RETURN2:
    case KEY_KP_ENTER:
        if (!readOnly_)
        {
            i32 pos = cursorPosition_;
            text_ = text_.SubstringUTF8(0, pos) + "\n" + text_.SubstringUTF8(pos);
            UpdateText();
            SetCursorPosition(pos + 1);
        }
        break;

    default:
        break;
    }
}

void MultiLineEdit::OnTextInput(const String& input)
{
    if (readOnly_ || input.Empty())
        return;

    i32 pos = cursorPosition_;
    text_ = text_.SubstringUTF8(0, pos) + input + text_.SubstringUTF8(pos);
    UpdateText();
    SetCursorPosition(pos + input.LengthUTF8());
}

void MultiLineEdit::SetText(const String& text)
{
    text_ = text;
    UpdateText();
    SetCursorPosition(text_.LengthUTF8());
}

void MultiLineEdit::AppendLine(const String& line)
{
    if (!text_.Empty() && !text_.EndsWith("\n"))
        text_ += "\n";
    text_ += line;
    numLines_ = CountLines(text_);

    // Trim oldest lines if over limit
    if (maxLines_ > 0 && numLines_ > maxLines_)
    {
        while (numLines_ > maxLines_)
        {
            unsigned pos = text_.Find('\n');
            if (pos != String::NPOS)
            {
                text_ = text_.Substring(pos + 1);
                --numLines_;
            }
            else
                break;
        }
    }

    UpdateText();

    // Auto-scroll: place cursor at end
    if (autoScroll_)
        SetCursorPosition(text_.LengthUTF8());
}

void MultiLineEdit::SetWordWrap(bool enable)
{
    if (textElement_)
        textElement_->SetWordwrap(enable);
}

void MultiLineEdit::SetCursorPosition(i32 position)
{
    i32 len = text_.LengthUTF8();
    if (position > len)
        position = len;
    if (position < 0)
        position = 0;

    cursorPosition_ = position;
    UpdateCursor();
}

String MultiLineEdit::GetSelectedText() const
{
    if (!textElement_)
        return String::EMPTY;

    i32 start = textElement_->GetSelectionStart();
    i32 length = textElement_->GetSelectionLength();
    if (length <= 0)
        return String::EMPTY;

    return text_.SubstringUTF8(start, length);
}

i32 MultiLineEdit::GetCharIndex(const IntVector2& position)
{
    if (!textElement_)
        return NINDEX;

    IntVector2 screenPosition = ElementToScreen(position);
    IntVector2 textPosition = textElement_->ScreenToElement(screenPosition);

    // Find nearest character by checking all character positions
    i32 numChars = textElement_->GetNumChars();
    if (numChars == 0)
        return 0;

    i32 bestIndex = 0;
    float bestDist = M_INFINITY;

    for (i32 i = 0; i <= numChars; ++i)
    {
        Vector2 charPos = textElement_->GetCharPosition(i);
        float dx = charPos.x_ - textPosition.x_;
        float dy = charPos.y_ - textPosition.y_;
        float dist = dx * dx + dy * dy;
        if (dist < bestDist)
        {
            bestDist = dist;
            bestIndex = i;
        }
    }

    return bestIndex;
}

void MultiLineEdit::UpdateCursor()
{
    if (!textElement_ || !cursor_)
        return;

    Vector2 charPos = textElement_->GetCharPosition(cursorPosition_);
    int rowHeight = textElement_->GetRowHeight();

    cursor_->SetPosition(static_cast<int>(charPos.x_), static_cast<int>(charPos.y_));
    cursor_->SetFixedSize(1, rowHeight);

    cursorBlinkTimer_ = 0.0f;
}

void MultiLineEdit::UpdateText()
{
    if (!textElement_)
        return;

    textElement_->SetText(text_);
    numLines_ = CountLines(text_);
}

unsigned MultiLineEdit::CountLines(const String& text)
{
    if (text.Empty())
        return 0;

    unsigned count = 1;
    for (unsigned i = 0; i < text.Length(); ++i)
    {
        if (text[i] == '\n')
            ++count;
    }
    return count;
}

bool MultiLineEdit::FilterImplicitAttributes(XMLElement& dest) const
{
    if (!BorderImage::FilterImplicitAttributes(dest))
        return false;

    XMLElement childElem = dest.GetChild("element");
    if (!childElem)
        return true;
    if (!RemoveChildXML(childElem, "Name", "MLE_Text"))
        return false;
    if (!RemoveChildXML(childElem, "Position"))
        return false;
    if (!RemoveChildXML(childElem, "Size"))
        return false;

    childElem = childElem.GetNext("element");
    if (!childElem)
        return true;
    if (!RemoveChildXML(childElem, "Name", "MLE_Cursor"))
        return false;

    return true;
}

void MultiLineEdit::HandleFocused(StringHash /*eventType*/, VariantMap& eventData)
{
    UpdateCursor();
}

void MultiLineEdit::HandleDefocused(StringHash /*eventType*/, VariantMap& eventData)
{
    cursor_->SetVisible(false);
}

} // namespace Urho3D
