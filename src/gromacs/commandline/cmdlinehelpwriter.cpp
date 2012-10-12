/*
 *
 *                This source code is part of
 *
 *                 G   R   O   M   A   C   S
 *
 *          GROningen MAchine for Chemical Simulations
 *
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2009, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 *
 * For more info, check our website at http://www.gromacs.org
 */
/*! \internal \file
 * \brief
 * Implements gmx::CommandLineHelpWriter.
 *
 * \author Teemu Murtola <teemu.murtola@cbr.su.se>
 * \ingroup module_commandline
 */
#include "cmdlinehelpwriter.h"

#include <string>

#include "gromacs/onlinehelp/helpformat.h"
#include "gromacs/onlinehelp/helpwritercontext.h"
#include "gromacs/options/basicoptioninfo.h"
#include "gromacs/options/filenameoptioninfo.h"
#include "gromacs/options/options.h"
#include "gromacs/options/optionsvisitor.h"
#include "gromacs/options/timeunitmanager.h"
#include "gromacs/selection/selectionfileoptioninfo.h"
#include "gromacs/selection/selectionoptioninfo.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/file.h"
#include "gromacs/utility/stringutil.h"

namespace gmx
{

namespace
{

/********************************************************************
 * DescriptionWriter
 */

/*! \internal \brief
 * Helper object for writing section descriptions to help.
 *
 * \ingroup module_commandline
 */
class DescriptionWriter : public OptionsVisitor
{
    public:
        //! Creates a helper object for writing section descriptions.
        explicit DescriptionWriter(const HelpWriterContext &context)
            : context_(context)
        {
        }

        virtual void visitSubSection(const Options &section);
        virtual void visitOption(const OptionInfo & /*option*/) { }

    private:
        const HelpWriterContext &context_;
};

void DescriptionWriter::visitSubSection(const Options &section)
{
    if (!section.description().empty())
    {
        File &file = context_.outputFile();
        const std::string &title = section.title();
        if (!title.empty())
        {
            file.writeLine(title);
            file.writeLine();
        }
        context_.writeTextBlock(section.description());
        file.writeLine();
    }
    OptionsIterator(section).acceptSubSections(this);
}


/********************************************************************
 * FileParameterWriter
 */

/*! \internal \brief
 * Helper object for writing help for file parameters.
 *
 * \ingroup module_commandline
 */
class FileParameterWriter : public OptionsTypeVisitor<FileNameOptionInfo>
{
    public:
        //! Creates a helper object for writing file parameters.
        explicit FileParameterWriter(const HelpWriterContext &context);

        //! Returns true if anything was written out.
        bool didOutput() const { return formatter_.didOutput(); }

        virtual void visitSubSection(const Options &section);
        virtual void visitOptionType(const FileNameOptionInfo &option);

    private:
        const HelpWriterContext &context_;
        TextTableFormatter       formatter_;
};

FileParameterWriter::FileParameterWriter(const HelpWriterContext &context)
    : context_(context)
{
    formatter_.addColumn("Option",      6, false);
    formatter_.addColumn("Filename",    12, false);
    formatter_.addColumn("Type",        12, false);
    formatter_.addColumn("Description", 45, true);
}

void FileParameterWriter::visitSubSection(const Options &section)
{
    OptionsIterator iterator(section);
    iterator.acceptSubSections(this);
    iterator.acceptOptions(this);
}

void FileParameterWriter::visitOptionType(const FileNameOptionInfo &option)
{
    int firstShortValue = 0; // The first value after which the type fits.
    int firstLongValue = -1; // First value that overlaps description column.
    int lastLongValue = -1;  // Last value like the above.

    // Get the values to write and check where text overflows the columns.
    formatter_.clear();
    std::string name(formatString("-%s", option.name().c_str()));
    formatter_.addColumnLine(0, name);
    for (int i = 0; i < option.valueCount() || i == 0; ++i)
    {
        std::string value;
        if (option.valueCount() == 0
            || (option.valueCount() == 1 && option.formatValue(0).empty()))
        {
            value = option.formatDefaultValueIfSet();
        }
        else
        {
            value = option.formatValue(i);
        }
        formatter_.addColumnLine(1, value);
        if (value.length() > 12U && i == firstShortValue)
        {
            firstShortValue = i + 1;
        }
        if (value.length() > 25U)
        {
            if (firstLongValue == -1)
            {
                firstLongValue = i;
            }
            lastLongValue = i;
        }
    }
    std::string type;
    if (option.isInputOutputFile())
    {
        type = "In/Out";
    }
    else if (option.isInputFile())
    {
        type = "Input";
    }
    else if (option.isOutputFile())
    {
        type = "Output";
    }
    if (!option.isRequired())
    {
        type += ", Opt.";
    }
    if (option.isLibraryFile())
    {
        type += ", Lib.";
    }
    bool bLongType = (type.length() > 12U);
    formatter_.addColumnLine(2, type);
    formatter_.addColumnLine(3, context_.substituteMarkup(option.description()));

    // Compute layout.
    if (name.length() > 6U || firstShortValue > 0)
    {
        formatter_.setColumnFirstLineOffset(1, 1);
        // Assume that the name is <20 chars, so that the type fits
        if (firstLongValue >= 0)
        {
            ++firstLongValue;
            ++lastLongValue;
        }
    }
    int firstDescriptionLine = 0;
    if (bLongType)
    {
        firstDescriptionLine = 1;
    }
    formatter_.setColumnFirstLineOffset(3, firstDescriptionLine);
    if (firstLongValue >= 0 && formatter_.lastColumnLine(3) >= firstLongValue)
    {
        firstDescriptionLine = lastLongValue + 1;
        formatter_.setColumnFirstLineOffset(3, firstDescriptionLine);
    }

    // Do the formatting.
    context_.outputFile().writeString(formatter_.formatRow());
}


/********************************************************************
 * ParameterWriter
 */

/*! \internal \brief
 * Helper object for writing help for non-file parameters.
 *
 * \ingroup module_commandline
 */
class ParameterWriter : public OptionsVisitor
{
    public:
        //! Creates a helper object for writing non-file parameters.
        ParameterWriter(const HelpWriterContext &context,
                        const char *timeUnit);

        //! Sets the writer to show hidden options.
        void setShowHidden(bool bSet) { bShowHidden_ = bSet; }
        //! Returns true if anything was written out.
        bool didOutput() const { return formatter_.didOutput(); }

        virtual void visitSubSection(const Options &section);
        virtual void visitOption(const OptionInfo &option);

    private:
        const HelpWriterContext &context_;
        TextTableFormatter       formatter_;
        const char              *timeUnit_;
        bool                     bShowHidden_;
};

ParameterWriter::ParameterWriter(const HelpWriterContext &context,
                                 const char *timeUnit)
    : context_(context), timeUnit_(timeUnit), bShowHidden_(false)
{
    formatter_.addColumn("Option",      12, false);
    formatter_.addColumn("Type",         6, false);
    formatter_.addColumn("Value",        6, false);
    formatter_.addColumn("Description", 51, true);
}

void ParameterWriter::visitSubSection(const Options &section)
{
    OptionsIterator iterator(section);
    iterator.acceptSubSections(this);
    iterator.acceptOptions(this);
}

void ParameterWriter::visitOption(const OptionInfo &option)
{
    if (option.isType<FileNameOptionInfo>()
        || option.isType<SelectionFileOptionInfo>()
        || option.isType<SelectionOptionInfo>()
        || (!bShowHidden_ && option.isHidden()))
    {
        return;
    }

    formatter_.clear();
    bool bIsBool = option.isType<BooleanOptionInfo>();
    std::string name(formatString("-%s%s", bIsBool ? "[no]" : "",
                                           option.name().c_str()));
    formatter_.addColumnLine(0, name);
    formatter_.addColumnLine(1, option.type());
    if (name.length() > 12U)
    {
        formatter_.setColumnFirstLineOffset(1, 1);
    }
    // TODO: Better handling of multiple long values
    std::string values;
    for (int i = 0; i < option.valueCount(); ++i)
    {
        if (i != 0)
        {
            values.append(" ");
        }
        values.append(option.formatValue(i));
    }
    formatter_.addColumnLine(2, values);
    std::string description(context_.substituteMarkup(option.description()));
    const DoubleOptionInfo *doubleOption = option.toType<DoubleOptionInfo>();
    if (doubleOption != NULL && doubleOption->isTime())
    {
        description = replaceAll(description, "%t", timeUnit_);
    }
    formatter_.addColumnLine(3, description);
    if (values.length() > 6U)
    {
        formatter_.setColumnFirstLineOffset(3, 1);
    }

    context_.outputFile().writeString(formatter_.formatRow());
}


/********************************************************************
 * SelectionParameterWriter
 */

/*! \internal \brief
 * Helper object for writing help for selection parameters.
 *
 * \ingroup module_commandline
 */
class SelectionParameterWriter : public OptionsVisitor
{
    public:
        //! Creates a helper object for writing selection parameters.
        explicit SelectionParameterWriter(const HelpWriterContext &context);

        //! Returns true if anything was written out.
        bool didOutput() const { return formatter_.didOutput(); }

        virtual void visitSubSection(const Options &section);
        virtual void visitOption(const OptionInfo &option);

    private:
        const HelpWriterContext &context_;
        TextTableFormatter       formatter_;
};

SelectionParameterWriter::SelectionParameterWriter(const HelpWriterContext &context)
    : context_(context)
{
    formatter_.addColumn("Selection",   10, false);
    formatter_.addColumn("Description", 67, true);
}

void SelectionParameterWriter::visitSubSection(const Options &section)
{
    OptionsIterator iterator(section);
    iterator.acceptSubSections(this);
    iterator.acceptOptions(this);
}

void SelectionParameterWriter::visitOption(const OptionInfo &option)
{
    if (!option.isType<SelectionFileOptionInfo>()
        && !option.isType<SelectionOptionInfo>())
    {
        return;
    }

    File &file = context_.outputFile();

    formatter_.clear();
    std::string name(formatString("-%s", option.name().c_str()));
    formatter_.addColumnLine(0, name);
    formatter_.addColumnLine(1, context_.substituteMarkup(option.description()));
    file.writeString(formatter_.formatRow());

    // TODO: What to do with selection variables?
    // They are not printed as values for any option.
    for (int i = 0; i < option.valueCount(); ++i)
    {
        std::string value(option.formatValue(i));
        // TODO: Wrapping
        file.writeLine(formatString("    %s", value.c_str()));
    }
}

} // namespace

/********************************************************************
 * CommandLineHelpWriter::Impl
 */

/*! \internal \brief
 * Private implementation class for CommandLineHelpWriter.
 *
 * \ingroup module_commandline
 */
class CommandLineHelpWriter::Impl
{
    public:
        //! Sets the Options object to use for generating help.
        explicit Impl(const Options &options);

        //! Options object to use for generating help.
        const Options          &options_;
        //! Time unit to show in descriptions.
        std::string             timeUnit_;
        //! Whether to write descriptions to output.
        bool                    bShowDescriptions_;
        //! Whether to write hidden options to output.
        bool                    bShowHidden_;
};

CommandLineHelpWriter::Impl::Impl(const Options &options)
    : options_(options), timeUnit_(TimeUnitManager().timeUnitAsString()),
      bShowDescriptions_(false), bShowHidden_(false)
{
}

/********************************************************************
 * CommandLineHelpWriter
 */

CommandLineHelpWriter::CommandLineHelpWriter(const Options &options)
    : impl_(new Impl(options))
{
}

CommandLineHelpWriter::~CommandLineHelpWriter()
{
}

CommandLineHelpWriter &CommandLineHelpWriter::setShowHidden(bool bSet)
{
    impl_->bShowHidden_ = bSet;
    return *this;
}

CommandLineHelpWriter &CommandLineHelpWriter::setShowDescriptions(bool bSet)
{
    impl_->bShowDescriptions_ = bSet;
    return *this;
}

CommandLineHelpWriter &CommandLineHelpWriter::setTimeUnitString(const char *timeUnit)
{
    impl_->timeUnit_ = timeUnit;
    return *this;
}

void CommandLineHelpWriter::writeHelp(const HelpWriterContext &context)
{
    if (context.outputFormat() != eHelpOutputFormat_Console)
    {
        // TODO: Implement once the situation with Redmine issue #969 is more
        // clear.
        GMX_THROW(NotImplementedError(
                    "Command-line help is not implemented for this output format"));
    }
    File &file = context.outputFile();
    if (impl_->bShowDescriptions_)
    {
        file.writeLine("DESCRIPTION");
        file.writeLine("-----------");
        file.writeLine();
        DescriptionWriter(context).visitSubSection(impl_->options_);
    }
    {
        FileParameterWriter writer(context);
        writer.visitSubSection(impl_->options_);
        if (writer.didOutput())
        {
            file.writeLine();
        }
    }
    {
        ParameterWriter writer(context, impl_->timeUnit_.c_str());
        writer.setShowHidden(impl_->bShowHidden_);
        writer.visitSubSection(impl_->options_);
        if (writer.didOutput())
        {
            file.writeLine();
        }
    }
    {
        SelectionParameterWriter writer(context);
        writer.visitSubSection(impl_->options_);
        if (writer.didOutput())
        {
            file.writeLine();
        }
    }
}

} // namespace gmx
