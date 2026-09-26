package com.queststoredb.perimeter

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.ActivityInfo
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Build
import android.provider.DocumentsContract
import android.provider.Settings
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextPaint
import android.text.method.LinkMovementMethod
import android.text.style.URLSpan
import android.view.Gravity
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import android.widget.ScrollView
import android.view.View

/** Selects content, then obtains the raw path required by the native engine. */
class ContentActivity : Activity() {
    private lateinit var storage: GameContentStorage
    private lateinit var play: Button
    private lateinit var buildNote: TextView
    private var launchOptionsEditor: GameLaunchOptionsEditor? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (isRunningOnHorizonOs()) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        }
        storage = GameContentStorage(this)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
            val padding = (24 * resources.displayMetrics.density).toInt()
            setPadding(
                padding,
                padding,
                padding,
                resources.getDimensionPixelSize(R.dimen.launcher_bottom_padding)
            )
        }
        play = Button(this).apply {
            setText(R.string.play)
            setOnClickListener { startGameIfReady() }
            measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, measuredHeight * 3 / 2)
        }
        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(getColor(R.color.launcher_controls_background))
                cornerRadius = MAIN_CONTROLS_CORNER_RADIUS_PX.toFloat()
            }
            elevation = dp(MAIN_CONTROLS_ELEVATION_DP).toFloat()
            layoutParams = LinearLayout.LayoutParams(dp(MAIN_CONTROLS_WIDTH_DP),
                LinearLayout.LayoutParams.WRAP_CONTENT)
        }
        controls.addView(play)
        controls.addView(Button(this).apply {
            setText(R.string.launch_arguments)
            setOnClickListener { showLaunchOptionsEditor() }
        }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ))
        controls.addView(Button(this).apply {
            setText(R.string.controls)
            setOnClickListener { showControls() }
        }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ))
        addResolutionScaleControl(controls)
        addFrameRateLimitControl(controls)
        layout.addView(controls)
        buildNote = TextView(this).apply {
            textSize = 12f
            setTextColor(Color.argb(204, 255, 255, 255))
            gravity = Gravity.END
            movementMethod = LinkMovementMethod.getInstance()
            setLayerType(View.LAYER_TYPE_SOFTWARE, null)
            val shadowOffset = resources.displayMetrics.density
            paint.setShadowLayer(
                shadowOffset,
                0f,
                shadowOffset,
                Color.argb(96, 0, 0, 0)
            )
        }
        layout.addView(buildNote, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ))
        GameLaunchOptions(this).ensureRendererSelected()
        refreshBuildNote()
        val content = ScrollView(this).apply {
            isFillViewport = true
            addView(layout)
        }
        setContentView(FrameLayout(this).apply {
            addView(ImageView(this@ContentActivity).apply {
                setImageResource(R.drawable.launcher_background)
                scaleType = ImageView.ScaleType.CENTER_CROP
                contentDescription = null
            }, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            ))
            addView(View(this@ContentActivity).apply {
                setBackgroundResource(R.drawable.launcher_vignette)
            }, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            ))
            addView(ImageView(this@ContentActivity).apply {
                setImageResource(R.drawable.launcher_logo)
                adjustViewBounds = true
                scaleType = ImageView.ScaleType.FIT_CENTER
                contentDescription = null
            }, FrameLayout.LayoutParams(
                minOf(dp(MAX_LOGO_WIDTH_DP),
                    resources.displayMetrics.widthPixels -
                        (resources.displayMetrics.widthPixels * LOGO_SIDE_MARGIN_RATIO).toInt() * 2),
                FrameLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
                topMargin = resources.getDimensionPixelSize(R.dimen.logo_top_margin)
            })
            addView(content, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            ))
        })
        refresh()
        if (savedInstanceState == null && Build.VERSION.SDK_INT == Build.VERSION_CODES.Q &&
            !storage.hasStorageAccess()) {
            requestLegacyStorageAccess()
        }
    }

    override fun onResume() {
        super.onResume()
        if (::storage.isInitialized) {
            refresh()
            launchOptionsEditor?.refreshContent()
            refreshBuildNote()
        }
    }

    private fun showLaunchOptionsEditor() {
        val options = GameLaunchOptions(this)
        val editor = GameLaunchOptionsEditor(this, options.load(), {
            options.save(it)
            refreshBuildNote()
        }, storage, ::chooseOrGrantAccess)
        launchOptionsEditor = editor
        editor.show()
    }

    private fun showControls() {
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), dp(8))
        }
        addControlsSection(content, R.string.controls_touch_section)
        addControlRow(content, R.string.controls_tap, R.string.controls_left_click)
        addControlRow(content, R.string.controls_two_finger_tap, R.string.controls_right_click)
        addControlRow(content, R.string.controls_two_finger_drag, R.string.controls_move_camera)
        addControlRow(content, R.string.controls_hold_two_fingers_drag, R.string.controls_rotate_camera)
        addControlRow(content, R.string.controls_pinch, R.string.controls_zoom)
        addControlsSection(content, R.string.controls_menu_section)
        addControlRow(content, R.string.controls_two_finger_scroll, R.string.controls_mouse_scroll)

        AlertDialog.Builder(this)
            .setTitle(R.string.controls)
            .setView(ScrollView(this).apply {
                isFillViewport = true
                addView(content)
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    minOf(dp(CONTROLS_DIALOG_MAX_HEIGHT_DP),
                        (resources.displayMetrics.heightPixels * CONTROLS_DIALOG_HEIGHT_RATIO).toInt())
                )
            })
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun addControlsSection(layout: LinearLayout, title: Int) {
        layout.addView(TextView(this).apply {
            setText(title)
            setTextAppearance(com.google.android.material.R.style.TextAppearance_MaterialComponents_Subtitle2)
            setPadding(0, dp(CONTROLS_SECTION_TOP_PADDING_DP), 0, dp(CONTROLS_SECTION_BOTTOM_PADDING_DP))
        }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ))
    }

    private fun addControlRow(layout: LinearLayout, gesture: Int, action: Int) {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(CONTROLS_ROW_VERTICAL_PADDING_DP), 0, dp(CONTROLS_ROW_VERTICAL_PADDING_DP))
        }
        row.addView(TextView(this).apply {
            setText(gesture)
            setTextAppearance(com.google.android.material.R.style.TextAppearance_MaterialComponents_Body2)
            gravity = Gravity.END
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(TextView(this).apply {
            setText(action)
            setTextAppearance(com.google.android.material.R.style.TextAppearance_MaterialComponents_Body2)
            gravity = Gravity.START
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
            marginStart = dp(CONTROLS_COLUMN_GAP_DP)
        })
        layout.addView(row)
    }

    private fun addResolutionScaleControl(layout: LinearLayout) {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        }
        val label = TextView(this).apply {
            text = "${getString(R.string.render_resolution)}:"
        }
        val choices = ResolutionScale.percentages.map { percent ->
            val size = ResolutionScale.renderSize(this, percent)
            getString(R.string.render_resolution_value, size.first, size.second)
        }
        val selector = Spinner(this).apply {
            adapter = ArrayAdapter(this@ContentActivity, android.R.layout.simple_spinner_item, choices).apply {
                setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            }
            contentDescription = getString(R.string.render_resolution)
            setSelection(ResolutionScale.percentages.indexOf(ResolutionScale.load(this@ContentActivity)).coerceAtLeast(0))
        }

        selector.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                ResolutionScale.save(this@ContentActivity, ResolutionScale.percentages[position])
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
        row.addView(label, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ))
        row.addView(selector, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ))
        layout.addView(row)
    }

    private fun addFrameRateLimitControl(layout: LinearLayout) {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        }
        row.addView(CheckBox(this).apply {
            text = getString(
                R.string.limit_frame_rate_to,
                FrameRateLimit.displayedFramesPerSecond(this@ContentActivity)
            )
            isChecked = FrameRateLimit.load(this@ContentActivity)
            setOnCheckedChangeListener { _, checked ->
                FrameRateLimit.save(this@ContentActivity, checked)
            }
        })
        layout.addView(row)
    }

    private fun refreshBuildNote() {
        val options = GameLaunchOptions(this)
        val renderer = when {
            options.renderer() == "sokol" -> "Sokol"
            options.dxvkVersion() == "1" -> "DXVK 1"
            options.dxvkVersion() == "2" -> "DXVK 2"
            else -> "DXVK version not selected"
        }
        buildNote.text = getString(
            R.string.build_note, BuildConfig.VERSION_NAME, BuildConfig.PERIMETER_VERSION
        ) + " • $renderer\n" + getString(R.string.android_port_by)
        val credit = getString(R.string.android_port_by)
        buildNote.text = SpannableStringBuilder(buildNote.text).apply {
            val creditStart = length - credit.length
            setSpan(
                object : URLSpan("https://github.com/Rush-iam/Perimeter-Android") {
                    override fun updateDrawState(ds: TextPaint) {
                        super.updateDrawState(ds)
                        ds.color = Color.LTGRAY
                        ds.isUnderlineText = true
                    }
                },
                creditStart,
                length,
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
            )
        }
    }

    private fun chooseOrGrantAccess() {
        if (!storage.hasStorageAccess()) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:$packageName")))
            } else {
                requestLegacyStorageAccess()
            }
            return
        }
        @Suppress("DEPRECATION")
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
            storage.selectedTree()?.let { putExtra(DocumentsContract.EXTRA_INITIAL_URI, it) }
        }, SELECT_CONTENT)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQUEST_STORAGE) return
        refresh()
        launchOptionsEditor?.refreshContent()
        if (grantResults.isNotEmpty() && !storage.hasStorageAccess() &&
            permissions.indices.any { grantResults[it] != android.content.pm.PackageManager.PERMISSION_GRANTED &&
                !shouldShowRequestPermissionRationale(permissions[it]) }) {
            AlertDialog.Builder(this)
                .setTitle(R.string.grant_legacy_access)
                .setMessage(R.string.storage_permission_settings)
                .setPositiveButton(R.string.open_app_settings) { _, _ ->
                    startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:$packageName")))
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    private fun requestLegacyStorageAccess() {
        val permissions = arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.WRITE_EXTERNAL_STORAGE)
        if (permissions.any { shouldShowRequestPermissionRationale(it) }) {
            AlertDialog.Builder(this)
                .setTitle(R.string.grant_legacy_access)
                .setMessage(R.string.storage_permission_rationale)
                .setPositiveButton(R.string.grant_legacy_access) { _, _ ->
                    requestPermissions(permissions, REQUEST_STORAGE)
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        } else {
            requestPermissions(permissions, REQUEST_STORAGE)
        }
    }

    @Deprecated("Uses the platform activity result API for the existing wrapper")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != SELECT_CONTENT || resultCode != RESULT_OK) return
        val tree = data?.data ?: return
        try {
            val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            contentResolver.takePersistableUriPermission(tree, flags)
            storage.select(tree)
            refresh()
            launchOptionsEditor?.refreshContent()
        } catch (error: Exception) {
            showContentError(error)
        }
    }

    private fun refresh() {
        val contentAvailable = if (!storage.hasStorageAccess()) {
            false
        } else {
            try {
                storage.localFilesystemPath() != null
            } catch (error: Exception) {
                false
            }
        }
        play.setText(if (contentAvailable) R.string.play else R.string.choose_content)
        play.isEnabled = true
        play.setOnClickListener {
            if (contentAvailable) startGameIfReady() else chooseOrGrantAccess()
        }
    }

    private fun startGameIfReady() {
        try {
            storage.localFilesystemPath() ?: error(getString(storageAccessMessage()))
            GameLaunchOptions(this).ensureRendererSelected()
            if (requiresVulkan13ForDxvk() && !hasVulkan13Support()) {
                showDxvk2UnsupportedDialog()
                return
            }
            startGame()
        } catch (error: Exception) {
            showContentError(error)
        }
    }

    private fun requiresVulkan13ForDxvk(): Boolean =
        GameLaunchOptions(this).let { options ->
            options.dxvkVersion() == "2" && options.renderer() == "d3d9"
        }

    private fun hasVulkan13Support(): Boolean = AndroidVulkanCapabilities.supportsVulkan13()

    private fun hasVulkan11Support(): Boolean = AndroidVulkanCapabilities.supportsVulkan11()

    private fun showDxvk2UnsupportedDialog() {
        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.dxvk2_vulkan_unsupported_title)
            .setMessage(R.string.dxvk2_vulkan_unsupported_message)
        if (hasVulkan11Support()) {
            dialog.setPositiveButton(R.string.use_dxvk1_renderer) { _, _ ->
                GameLaunchOptions(this).selectDxvkVersion("1")
                startGame()
            }
        }
        dialog.setNeutralButton(R.string.use_sokol_renderer) { _, _ ->
                GameLaunchOptions(this).selectRenderer("sokol")
                startGame()
        }
        dialog.setNegativeButton(android.R.string.cancel, null).show()
    }

    private fun startGame() {
        try {
            val questVrLaunch = BuildConfig.FLAVOR == "quest"
            val gameActivity = if (questVrLaunch) QuestGameActivity::class.java else MainActivity::class.java
            startActivity(Intent(this, gameActivity).apply {
                if (BuildConfig.DEBUG && questVrLaunch)
                    putExtra("quest_test_mission", intent.getStringExtra("quest_test_mission"))
                if (questVrLaunch) {
                    addCategory("com.oculus.intent.category.VR")
                    addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                }
            })
            finish()
        } catch (error: Exception) {
            showContentError(error)
        }
    }

    private fun showContentError(error: Exception) {
        AlertDialog.Builder(this)
            .setTitle(R.string.choose_content)
            .setMessage(getString(R.string.content_error, error.localizedMessage))
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun storageAccessMessage(): Int =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) R.string.grant_storage_access
        else R.string.grant_legacy_storage_access

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private fun isRunningOnHorizonOs(): Boolean =
        packageManager.hasSystemFeature(FEATURE_HORIZON_OS) ||
            packageManager.hasSystemFeature(FEATURE_STANDALONE_VR)

    private companion object {
        const val MAIN_CONTROLS_WIDTH_DP = 300
        const val MAIN_CONTROLS_CORNER_RADIUS_PX = 6
        const val MAIN_CONTROLS_ELEVATION_DP = 8
        const val CONTROLS_DIALOG_MAX_HEIGHT_DP = 440
        const val CONTROLS_DIALOG_HEIGHT_RATIO = 0.6f
        const val CONTROLS_SECTION_TOP_PADDING_DP = 12
        const val CONTROLS_SECTION_BOTTOM_PADDING_DP = 4
        const val CONTROLS_ROW_VERTICAL_PADDING_DP = 6
        const val CONTROLS_COLUMN_GAP_DP = 12
        const val MAX_LOGO_WIDTH_DP = 500
        const val LOGO_SIDE_MARGIN_RATIO = 0.10f
        const val SELECT_CONTENT = 1
        const val REQUEST_STORAGE = 2
        const val FEATURE_HORIZON_OS = "horizonos.software.horizon_os"
        const val FEATURE_STANDALONE_VR = "oculus.hardware.standalone_vr"
    }
}
