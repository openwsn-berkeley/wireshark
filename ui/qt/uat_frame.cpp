/* uat_frame.cpp
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

#include "config.h"

#include <glib.h>

#include <epan/filter_expressions.h>

#include "uat_frame.h"
#include <ui_uat_frame.h>
#include "display_filter_edit.h"
#include "wireshark_application.h"

#include "qt_ui_utils.h"
#include <wsutil/report_message.h>

#include <QLineEdit>
#include <QKeyEvent>
#include <QTreeWidgetItemIterator>

#include <QDebug>

UatFrame::UatFrame(QWidget *parent) :
    QFrame(parent),
    ui(new Ui::UatFrame),
    uat_model_(NULL),
    uat_delegate_(NULL),
    uat_(NULL)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC
    ui->newToolButton->setAttribute(Qt::WA_MacSmallSize, true);
    ui->deleteToolButton->setAttribute(Qt::WA_MacSmallSize, true);
    ui->copyToolButton->setAttribute(Qt::WA_MacSmallSize, true);
    ui->pathLabel->setAttribute(Qt::WA_MacSmallSize, true);
#endif

    // FIXME: this prevents the columns from being resized, even if the text
    // within a combobox needs more space (e.g. in the USER DLT settings).  For
    // very long filenames in the SSL RSA keys dialog, it also results in a
    // vertical scrollbar. Maybe remove this since the editor is not limited to
    // the column width (and overlays other fields if more width is needed)?
#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
    ui->uatTreeView->header()->setResizeMode(QHeaderView::ResizeToContents);
#else
    ui->uatTreeView->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
#endif

    // XXX - Need to add uat_move or uat_insert to the UAT API for drag/drop
}

UatFrame::~UatFrame()
{
    delete ui;
    delete uat_delegate_;
    delete uat_model_;
}

void UatFrame::setUat(epan_uat *uat)
{
    QString title(tr("Unknown User Accessible Table"));

    uat_ = uat;

    ui->pathLabel->clear();
    ui->pathLabel->setEnabled(false);

    if (uat_) {
        if (uat_->name) {
            title = uat_->name;
        }

        QString abs_path = gchar_free_to_qstring(uat_get_actual_filename(uat_, FALSE));
        ui->pathLabel->setText(abs_path);
        ui->pathLabel->setUrl(QUrl::fromLocalFile(abs_path).toString());
        ui->pathLabel->setToolTip(tr("Open ") + uat->filename);
        ui->pathLabel->setEnabled(true);

        uat_model_ = new UatModel(NULL, uat);
        uat_delegate_ = new UatDelegate;
        ui->uatTreeView->setModel(uat_model_);
        ui->uatTreeView->setItemDelegate(uat_delegate_);

        connect(uat_model_, SIGNAL(dataChanged(QModelIndex,QModelIndex)),
                this, SLOT(modelDataChanged(QModelIndex)));
        connect(uat_model_, SIGNAL(rowsRemoved(QModelIndex, int, int)),
                this, SLOT(modelRowsRemoved()));
        connect(ui->uatTreeView, SIGNAL(currentItemChanged(QModelIndex,QModelIndex)),
                this, SLOT(viewCurrentChanged(QModelIndex,QModelIndex)));
    }

    setWindowTitle(title);
}

void UatFrame::acceptChanges()
{
    if (!uat_) return;

    if (uat_->changed) {
        gchar *err = NULL;

        if (!uat_save(uat_, &err)) {
            report_failure("Error while saving %s: %s", uat_->name, err);
            g_free(err);
        }

        if (uat_->post_update_cb) {
            uat_->post_update_cb();
        }

        //Filter expressions don't affect dissection, so there is no need to
        //send any events to that effect.  However, the app needs to know
        //about any button changes.
        wsApp->emitAppSignal(WiresharkApplication::FilterExpressionsChanged);
    }
}

void UatFrame::rejectChanges()
{
    if (!uat_) return;

    if (uat_->changed) {
        gchar *err = NULL;
        uat_clear(uat_);
        if (!uat_load(uat_, &err)) {
            report_failure("Error while loading %s: %s", uat_->name, err);
            g_free(err);
        }
        //Filter expressions don't affect dissection, so there is no need to
        //send any events to that effect
    }
}

void UatFrame::addRecord(bool copy_from_current)
{
    if (!uat_) return;

    const QModelIndex &current = ui->uatTreeView->currentIndex();
    if (copy_from_current && !current.isValid()) return;

    // should not fail, but you never know.
    if (!uat_model_->insertRows(uat_model_->rowCount(), 1)) {
        qDebug() << "Failed to add a new record";
        return;
    }
    const QModelIndex &new_index = uat_model_->index(uat_model_->rowCount() - 1, 0);
    if (copy_from_current) {
        uat_model_->copyRow(new_index.row(), current.row());
    }
    // due to an EditTrigger, this will also start editing.
    ui->uatTreeView->setCurrentIndex(new_index);
    // trigger updating error messages and the OK button state.
    modelDataChanged(new_index);
}

// Invoked when a different field is selected. Note: when selecting a different
// field after editing, this event is triggered after modelDataChanged.
void UatFrame::viewCurrentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    if (current.isValid()) {
        ui->deleteToolButton->setEnabled(true);
        ui->copyToolButton->setEnabled(true);
    } else {
        ui->deleteToolButton->setEnabled(false);
        ui->copyToolButton->setEnabled(false);
    }

    checkForErrorHint(current, previous);
}

// Invoked when a field in the model changes (e.g. by closing the editor)
void UatFrame::modelDataChanged(const QModelIndex &topLeft)
{
    checkForErrorHint(topLeft, QModelIndex());
}

// Invoked after a row has been removed from the model.
void UatFrame::modelRowsRemoved()
{
    const QModelIndex &current = ui->uatTreeView->currentIndex();
    checkForErrorHint(current, QModelIndex());
}

// If the current field has errors, show them.
// Otherwise if the row has not changed, but the previous field has errors, show them.
// Otherwise pick the first error in the current row.
// Otherwise show the error from the previous field (if any).
// Otherwise clear the error hint.
void UatFrame::checkForErrorHint(const QModelIndex &current, const QModelIndex &previous)
{
    if (current.isValid()) {
        if (trySetErrorHintFromField(current)) {
            return;
        }

        const int row = current.row();
        if (row == previous.row() && trySetErrorHintFromField(previous)) {
            return;
        }

        for (int i = 0; i < uat_model_->columnCount(); i++) {
            if (trySetErrorHintFromField(uat_model_->index(row, i))) {
                return;
            }
        }
    }

    if (previous.isValid()) {
        if (trySetErrorHintFromField(previous)) {
            return;
        }
    }

    ui->hintLabel->clear();
}

bool UatFrame::trySetErrorHintFromField(const QModelIndex &index)
{
    const QVariant &data = uat_model_->data(index, Qt::UserRole + 1);
    if (!data.isNull()) {
        // use HTML instead of PlainText because that handles wordwrap properly
        ui->hintLabel->setText("<small><i>" + html_escape(data.toString()) + "</i></small>");
        return true;
    }
    return false;
}

void UatFrame::on_newToolButton_clicked()
{
    addRecord();
}

void UatFrame::on_deleteToolButton_clicked()
{
    const QModelIndex &current = ui->uatTreeView->currentIndex();
    if (uat_model_ && current.isValid()) {
        if (!uat_model_->removeRows(current.row(), 1)) {
            qDebug() << "Failed to remove row";
        }
    }
}

void UatFrame::on_copyToolButton_clicked()
{
    addRecord(true);
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
