/* tap_parameter_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * @file Tap parameter dialog class
 *
 * Base class for statistics dialogs. Subclasses must implement:
 * - fillTree. Called when the dialog is first displayed and when a display
 *   filter is applied. In most cases the subclass should clear the tree and
 *   retap packets here.
 * - filterExpression. If the subclass supports filtering context menu items
 *   ("Apply As Filter", etc.) it should fill in ctx_menu_ and implement
 *   filterExpression.
 * - getTreeAsString or treeItemData. Used for "Copy" and "Save As...".
 * -
 */

#include "tap_parameter_dialog.h"
#include <ui_tap_parameter_dialog.h>

#include <errno.h>

#include "epan/stat_tap_ui.h"

#include "ui/last_open_dir.h"
#include "ui/utf8_entities.h"

#include "wsutil/file_util.h"

#include "wireshark_application.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QMessageBox>
#include <QFileDialog>
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
// Qt::escape
#include <QTextDocument>
#endif

// The GTK+ counterpart uses tap_param_dlg, which we don't use. If we
// need tap parameters we should probably create a TapParameterDialog
// class based on WiresharkDialog and subclass it here.

// To do:
// - Add tap parameters? SCSI SRT uses PARAM_ENUM. Everything appears to use
//   PARAM_FILTER. Nothing uses _UINT, _STRING, or _UUID.
// - Update to match bug 9452 / r53657.
// - Create a TapParameterTreeWidgetItem class?
// - Better / more usable XML output.

const int expand_all_threshold_ = 100; // Arbitrary

static QHash<const QString, tpdCreator> cfg_str_to_creator_;

TapParameterDialog::TapParameterDialog(QWidget &parent, CaptureFile &cf, int help_topic) :
    WiresharkDialog(parent, cf),
    ui(new Ui::TapParameterDialog),
    help_topic_(help_topic)
{
    ui->setupUi(this);

    // XXX Use recent settings instead
    resize(parent.width() * 2 / 3, parent.height() * 3 / 4);

    ctx_menu_.addAction(ui->actionCopyToClipboard);
    ctx_menu_.addAction(ui->actionSaveAs);

    QPushButton *button;
    button = ui->buttonBox->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(button, SIGNAL(clicked()), this, SLOT(on_actionCopyToClipboard_triggered()));

    button = ui->buttonBox->addButton(tr("Save as..."), QDialogButtonBox::ActionRole);
    connect(button, SIGNAL(clicked()), this, SLOT(on_actionSaveAs_triggered()));

    if (help_topic_ < 1) {
        ui->buttonBox->button(QDialogButtonBox::Help)->hide();
    }
}

TapParameterDialog::~TapParameterDialog()
{
    delete ui;
}

void TapParameterDialog::registerDialog(const QString title, const char *cfg_abbr, register_stat_group_t group, stat_tap_init_cb tap_init_cb, tpdCreator creator)
{
    stat_tap_ui ui_info;

    ui_info.group = group;
    ui_info.title = title.toUtf8().constData();
    ui_info.cli_string = cfg_abbr;
    ui_info.tap_init_cb = tap_init_cb;
    ui_info.nparams = 0; // We'll need this for SCSI SRT
    ui_info.params = NULL;
    register_stat_tap_ui(&ui_info, NULL);

    QString cfg_str = cfg_abbr;
    cfg_str_to_creator_[cfg_str] = creator;

    QAction *tpd_action = new QAction(title, NULL);
    tpd_action->setData(cfg_str);
    wsApp->addStatisticsGroupItem(group, tpd_action);
}

TapParameterDialog *TapParameterDialog::showTapParameterStatistics(QWidget &parent, CaptureFile &cf, const QString cfg_str, const QString arg, void *)
{
    if (cfg_str_to_creator_.contains(cfg_str)) {
        TapParameterDialog *tpd = cfg_str_to_creator_[cfg_str](parent, cfg_str, arg, cf);
        return tpd;
    }
    return NULL;
}

QTreeWidget *TapParameterDialog::statsTreeWidget()
{
    return ui->statsTreeWidget;
}

const char *TapParameterDialog::displayFilter()
{
    return ui->displayFilterLineEdit->text().toUtf8().constData();
}

// This assumes that we're called before signals are connected or show()
// is called.
void TapParameterDialog::setDisplayFilter(const QString &filter)
{
    ui->displayFilterLineEdit->setText(filter);
}

void TapParameterDialog::filterActionTriggered()
{
    FilterAction *fa = qobject_cast<FilterAction *>(QObject::sender());
    QString filter_expr = filterExpression();

    if (!fa || filter_expr.isEmpty()) {
        return;
    }

    emit filterAction(filter_expr, fa->action(), fa->actionType());
}

QString TapParameterDialog::itemDataToPlain(QVariant var, int width)
{
    QString plain_str;
    int align_mul = 1;

    switch (var.type()) {
    case QVariant::String:
        align_mul = -1;
        // Fall through
    case QVariant::Int:
    case QVariant::UInt:
        plain_str = var.toString();
        break;
    case QVariant::Double:
        plain_str = QString::number(var.toDouble(), 'f', 6);
        break;
    default:
        break;
    }

    if (plain_str.length() < width) {
        plain_str = QString("%1").arg(plain_str, width * align_mul);
    }
    return plain_str;
}

QList<QVariant> TapParameterDialog::treeItemData(QTreeWidgetItem *) const
{
    return QList<QVariant>();
}

const QString plain_sep_ = "  ";
QByteArray TapParameterDialog::getTreeAsString(st_format_type format)
{
    QByteArray ba;
    QTreeWidgetItemIterator it(ui->statsTreeWidget, QTreeWidgetItemIterator::NotHidden);

    QList<int> col_widths;
    QByteArray footer;

    // Title + header
    switch (format) {
    case ST_FORMAT_PLAIN:
    {
        QTreeWidgetItemIterator width_it(it);
        QString plain_header;
        while (*width_it) {
            QList<QVariant> tid = treeItemData((*width_it));
            int col = 0;
            foreach (QVariant var, tid) {
                if (col_widths.size() <= col) {
                    col_widths.append(ui->statsTreeWidget->headerItem()->text(col).length());
                }
                if (var.type() == QVariant::String) {
                    col_widths[col] = qMax(col_widths[col], itemDataToPlain(var).length());
                }
                col++;
            }
            ++width_it;
        }
        QStringList ph_parts;
        for (int col = 0; col < ui->statsTreeWidget->columnCount() && col < col_widths.length(); col++) {
            ph_parts << ui->statsTreeWidget->headerItem()->text(col);
        }
        plain_header = ph_parts.join(plain_sep_);

        QByteArray top_separator;
        top_separator.fill('=', plain_header.length());
        top_separator.append('\n');
        QString file_header = QString("%1 - %2:\n").arg(windowSubtitle(), cap_file_.fileName());
        footer.fill('-', plain_header.length());
        footer.append('\n');
        plain_header.append('\n');

        ba.append(top_separator);
        ba.append(file_header);
        ba.append(plain_header);
        ba.append(footer);
        break;
    }
    case ST_FORMAT_CSV:
    {
        QString csv_header;
        QStringList ch_parts;
        for (int col = 0; col < ui->statsTreeWidget->columnCount(); col++) {
            ch_parts << QString("\"%1\"").arg(ui->statsTreeWidget->headerItem()->text(col));
        }
        csv_header = ch_parts.join(",");
        csv_header.append('\n');
        ba.append(csv_header.toUtf8().constData());
        break;
    }
    case ST_FORMAT_XML:
    {
        // XXX What's a useful format? This mostly conforms to DocBook.
        ba.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        QString title;
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
        title = Qt::escape(windowSubtitle());
#else
        title = QString(windowSubtitle()).toHtmlEscaped();
#endif
        QString xml_header = QString("<table>\n<title>%1</title>\n").arg(title);
        ba.append(xml_header.toUtf8());
        ba.append("<thead>\n<row>\n");
        for (int col = 0; col < ui->statsTreeWidget->columnCount(); col++) {
            title = ui->statsTreeWidget->headerItem()->text(col);
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
            title = Qt::escape(title);
#else
            title = title.toHtmlEscaped();
#endif
            title = QString("  <entry>%1</entry>\n").arg(title);
            ba.append(title.toUtf8());
        }
        ba.append("</row>\n</thead>\n");
        ba.append("<tbody>\n");
        footer = "</tbody>\n</table>\n";
        break;
    }
    case ST_FORMAT_YAML:
    {
        QString yaml_header;
        ba.append("---\n");
        yaml_header = QString("Description: \"%1\"\nFile: \"%2\"\nItems:\n").arg(windowSubtitle()).arg(cap_file_.fileName());
        ba.append(yaml_header.toUtf8());
        break;
    }
    default:
        break;
    }

    // Data
    while (*it) {
        QList<QVariant> tid = treeItemData((*it));
        if (tid.length() < 1) {
            ++it;
            continue;
        }

        if (tid.length() < ui->statsTreeWidget->columnCount()) {
            // Assume we have a header
        }

        // Assume var length == columnCount
        QString line;
        QStringList parts;

        switch (format) {
        case ST_FORMAT_PLAIN:
        {
            int i = 0;
            foreach (QVariant var, tid) {
                parts << itemDataToPlain(var, col_widths[i]);
                i++;
            }
            line = parts.join(plain_sep_);
            line.append('\n');
            break;
        }
        case ST_FORMAT_CSV:
            foreach (QVariant var, tid) {
                if (var.type() == QVariant::String) {
                    parts << QString("\"%1\"").arg(var.toString());
                } else {
                    parts << var.toString();
                }
            }
            line = parts.join(",");
            line.append('\n');
            break;
        case ST_FORMAT_XML:
        {
            line = "<row>\n";
            foreach (QVariant var, tid) {
                QString entry;
    #if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
                entry = Qt::escape(var.toString());
    #else
                entry = var.toString().toHtmlEscaped();
    #endif
                line.append(QString("  <entry>%1</entry>\n").arg(entry));
            }
            line.append("</row>\n");
            break;
        }
        case ST_FORMAT_YAML:
        {
            int col = 0;
            QString indent = "-";
            foreach (QVariant var, tid) {
                QString entry;
                if (var.type() == QVariant::String) {
                    entry = QString("\"%1\"").arg(var.toString());
                } else {
                    entry = var.toString();
                }
                line.append(QString("  %1 %2: %3\n").arg(indent).arg(ui->statsTreeWidget->headerItem()->text(col), entry));
                indent = " ";
                col++;
            }
            break;
        }
        default:
            break;
        }

        ba.append(line.toUtf8());
        ++it;
    }

    // Footer
    ba.append(footer); // plain only?
    return ba;
}

void TapParameterDialog::drawTreeItems()
{
    if (ui->statsTreeWidget->model()->rowCount() < expand_all_threshold_) {
        ui->statsTreeWidget->expandAll();
    }

    for (int col = 0; col < ui->statsTreeWidget->columnCount(); col++) {
        ui->statsTreeWidget->resizeColumnToContents(col);
    }
}

void TapParameterDialog::showEvent(QShowEvent *)
{
    if (!ui->displayFilterLineEdit->text().isEmpty()) {
        QString filter = ui->displayFilterLineEdit->text();
        emit updateFilter(filter, true);
    }
    fillTree();
}

void TapParameterDialog::contextMenuEvent(QContextMenuEvent *event)
{
    bool enable = filterExpression().length() > 0 ? true : false;

    foreach (QAction *fa, filter_actions_) {
        fa->setEnabled(enable);
    }

    ctx_menu_.exec(event->globalPos());
}

void TapParameterDialog::updateWidgets()
{
    if (file_closed_) {
        ui->displayFilterLineEdit->setEnabled(false);
        ui->applyFilterButton->setEnabled(false);
    }
}

void TapParameterDialog::on_applyFilterButton_clicked()
{
    QString filter = ui->displayFilterLineEdit->text();
    emit updateFilter(filter, true);
    fillTree();
}

void TapParameterDialog::on_actionCopyToClipboard_triggered()
{
    wsApp->clipboard()->setText(getTreeAsString(ST_FORMAT_PLAIN));
}

void TapParameterDialog::on_actionSaveAs_triggered()
{
    QString selectedFilter;
    st_format_type format;
    const char *file_ext;
    FILE *f;
    bool success = false;
    int last_errno;

    QFileDialog SaveAsDialog(this, wsApp->windowTitleString(tr("Save Statistics As" UTF8_HORIZONTAL_ELLIPSIS)),
                                                            get_last_open_dir());
    SaveAsDialog.setNameFilter(tr("Plain text file (*.txt);;"
                                    "Comma separated values (*.csv);;"
                                    "XML document (*.xml);;"
                                    "YAML document (*.yaml)"));
    SaveAsDialog.selectNameFilter(tr("Plain text file (*.txt)"));
    SaveAsDialog.setAcceptMode(QFileDialog::AcceptSave);
    if (!SaveAsDialog.exec()) {
        return;
    }
    selectedFilter= SaveAsDialog.selectedNameFilter();
    if (selectedFilter.contains("*.yaml", Qt::CaseInsensitive)) {
        format = ST_FORMAT_YAML;
        file_ext = ".yaml";
    }
    else if (selectedFilter.contains("*.xml", Qt::CaseInsensitive)) {
        format = ST_FORMAT_XML;
        file_ext = ".xml";
    }
    else if (selectedFilter.contains("*.csv", Qt::CaseInsensitive)) {
        format = ST_FORMAT_CSV;
        file_ext = ".csv";
    }
    else {
        format = ST_FORMAT_PLAIN;
        file_ext = ".txt";
    }

    // Get selected filename and add extension of necessary
    QString file_name = SaveAsDialog.selectedFiles()[0];
    if (!file_name.endsWith(file_ext, Qt::CaseInsensitive)) {
        file_name.append(file_ext);
    }

    QByteArray tree_as_ba = getTreeAsString(format);

    // actually save the file
    f = ws_fopen (file_name.toUtf8().constData(), "w");
    last_errno = errno;
    if (f) {
        if (fputs(tree_as_ba.data(), f) != EOF) {
            success = true;
        }
        last_errno = errno;
        fclose(f);
    }
    if (!success) {
        QMessageBox::warning(this, tr("Error saving file %1").arg(file_name),
                             g_strerror (last_errno));
    }
}

void TapParameterDialog::on_buttonBox_helpRequested()
{
    if (help_topic_ > 0) {
        wsApp->helpTopicAction((topic_action_e) help_topic_);
    }
}

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */